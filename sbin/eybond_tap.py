#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
eybond_tap.py - прозрачный TCP-прокси ("tap") для даталоггера SmartESS/Eybond.

Стик сам подключается к облаку (ess.eybond.com -> 8.218.126.236). DNS-ответ для
стика направляем на хост с этим скриптом; tap принимает соединение стика,
открывает своё соединение к НАСТОЯЩЕМУ серверу (только IP:PORT, имя никогда не
резолвится - чтобы не попасть в петлю на подменённый DNS) и копирует байты в обе
стороны без изменений. Tap никогда не модифицирует, не задерживает и не
отвечает за какую-либо из сторон.

Направления в логе:
  S->C  от облачного сервера к стику
  C->S  от стика к облачному серверу

Пример:
  eybond_tap.py --listen 0.0.0.0:502 --upstream 8.218.126.236:502 --log tap.log
(порт 502 подтверждён захватом: стик шлёт SYN на :502; --upstream принимает и имя: ess.eybond.com:502 --dns 8.8.8.8)
"""
import argparse
import asyncio
import datetime
import ipaddress
import itertools
import os
import struct
import time
import signal
import socket
import sys

CHUNK = 65536          # размер одного чтения
CONNECT_TIMEOUT = 15.0  # таймаут подключения к upstream, с
WRITE_BUF_HIGH = 256 * 1024  # high-water для буфера записи (backpressure через drain)


# --- резолв имени upstream напрямую у внешнего DNS (минимальный клиент A-записей, только stdlib) ---
# Нужен, когда локальный DNS подменяет имя облака на адрес этого хоста: общий резолвер дал бы петлю.
def _dns_query(name, server, port=53, timeout=3.0):
    """Возвращает (список IPv4-строк, ttl). Бросает OSError/ValueError."""
    qid = struct.unpack('>H', os.urandom(2))[0]
    q = struct.pack('>HHHHHH', qid, 0x0100, 1, 0, 0, 0)
    for lab in name.rstrip('.').split('.'):
        b = lab.encode('ascii')
        if not 0 < len(b) < 64:
            raise ValueError('bad name %r' % name)
        q += bytes([len(b)]) + b
    q += b'\x00' + struct.pack('>HH', 1, 1)
    last = None
    for _ in range(2):
        sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        sock.settimeout(timeout)
        try:
            sock.sendto(q, (server, port))
            data, addr = sock.recvfrom(1500)
        except OSError as e:
            last = e
            continue
        finally:
            sock.close()
        if addr[0] != server or len(data) < 12 or struct.unpack('>H', data[:2])[0] != qid:
            last = ValueError('unexpected DNS reply')
            continue
        flags, qd, an = struct.unpack('>HHH', data[2:8])
        if flags & 0x8000 == 0 or (flags & 0xF) != 0:
            raise ValueError('DNS rcode %d' % (flags & 0xF))
        pos = 12

        def skip_name(i):
            while True:
                if i >= len(data):
                    raise ValueError('truncated')
                n = data[i]
                if n == 0:
                    return i + 1
                if n & 0xC0 == 0xC0:
                    return i + 2
                i += 1 + n
        for _q in range(qd):
            pos = skip_name(pos) + 4
        ips, ttl = [], None
        for _a in range(an):
            pos = skip_name(pos)
            if pos + 10 > len(data):
                raise ValueError('truncated')
            rtype, _cls, rttl, rdlen = struct.unpack('>HHIH', data[pos:pos + 10])
            pos += 10
            if rtype == 1 and rdlen == 4:
                ips.append('.'.join(str(x) for x in data[pos:pos + 4]))
                ttl = rttl if ttl is None else min(ttl, rttl)
            pos += rdlen
        if not ips:
            raise ValueError('no A records for %s' % name)
        return ips, ttl
    raise last or OSError('DNS timeout')


class Upstream:
    """Адрес облака: IP-литерал как есть, имя - через --dns (кэш по TTL, при сбое прежний адрес)."""

    def __init__(self, host, port, dns_server='8.8.8.8', dns_port=53):
        self.host, self.port, self.dns_server, self.dns_port = host, port, dns_server, dns_port
        try:
            ipaddress.ip_address(host)
            self.literal = True
        except ValueError:
            self.literal = False
        self.ip, self.expires = (host if self.literal else None), 0.0

    async def get(self, log=None):
        if self.literal or (self.ip and time.monotonic() < self.expires):
            return self.ip
        try:
            ips, ttl = await asyncio.get_event_loop().run_in_executor(
                None, _dns_query, self.host, self.dns_server, self.dns_port)
            ip = ips[0]
            if ipaddress.ip_address(ip).is_private or ipaddress.ip_address(ip).is_loopback:
                raise ValueError('resolved to private address %s (DNS override?)' % ip)
            self.ip, self.expires = ip, time.monotonic() + min(max(ttl or 60, 30), 300)
            if log:
                log.line('RESOLVE %s -> %s via %s (ttl=%s)' % (self.host, ip, self.dns_server, ttl))
        except (OSError, ValueError) as e:
            if log:
                log.line('RESOLVE-FAIL %s via %s: %r%s' % (self.host, self.dns_server, e,
                                                          ' (using last %s)' % self.ip if self.ip else ''))
            if not self.ip:
                raise OSError('cannot resolve %s' % self.host)
            self.expires = time.monotonic() + 30
        return self.ip


def utc_now():
    return datetime.datetime.now(datetime.timezone.utc)


def fmt_ts(dt):
    return dt.strftime('%Y-%m-%d %H:%M:%S.') + '%03d' % (dt.microsecond // 1000)


class Log:
    def __init__(self, path):
        self.fh = open(path, 'a', encoding='utf-8') if path else sys.stdout

    def line(self, msg):
        try:
            self.fh.write('%s %s\n' % (fmt_ts(utc_now()), msg))
            self.fh.flush()
        except Exception:
            pass  # сбой логирования не должен ронять проксирование

    def close(self):
        if self.fh is not sys.stdout:
            try:
                self.fh.close()
            except Exception:
                pass


def parse_hostport(s, what, min_port=1):
    host, sep, port = s.rpartition(':')
    if not sep or not port.isdigit() or not min_port <= int(port) < 65536:
        raise argparse.ArgumentTypeError('%s: ожидается HOST:PORT, получено %r' % (what, s))
    return host or '0.0.0.0', int(port)


def hex_head(data, n):
    s = data[:n].hex(' ')
    return s + (' ...' if len(data) > n else '')


def guess_header(data):
    """Эвристика (НЕ проверенный формат): первые 8 байт как 4 поля uint16 BE.
    Нужна только чтобы глазами искать закономерности; не использовать как факт."""
    if len(data) < 8:
        return ''
    f = [int.from_bytes(data[i:i + 2], 'big') for i in range(0, 8, 2)]
    return ' [heur u16be: %s]' % ' '.join('%04x' % x for x in f)


class Burst:
    """Группировка S->C чанков во всплески (пауза > gap открывает новый)."""

    def __init__(self, log, gap):
        self.log, self.gap = log, gap
        self.prev_start = None   # monotonic начала предыдущего всплеска
        self.cur = None          # [start_dt, start_mono, last_mono, chunks, bytes, since_prev]
        self.timer = None

    def add(self, nbytes):
        loop = asyncio.get_running_loop()
        now = loop.time()
        if self.cur and now - self.cur[2] > self.gap:
            self.close()
        if not self.cur:
            since = '%.1f' % (now - self.prev_start) if self.prev_start is not None else 'n/a'
            self.cur = [utc_now(), now, now, 0, 0, since]
            self.prev_start = now
        c = self.cur
        c[2] = now
        c[3] += 1
        c[4] += nbytes
        if self.timer:
            self.timer.cancel()
        self.timer = loop.call_later(self.gap, self.close)

    def close(self):
        if self.timer:
            self.timer.cancel()
            self.timer = None
        c, self.cur = self.cur, None
        if c:
            self.log.line('BURST start=%s dur=%.3f chunks=%d bytes=%d since_prev_start=%s'
                          % (fmt_ts(c[0]), c[2] - c[1], c[3], c[4], c[5]))


def set_keepalive(writer):
    sock = writer.get_extra_info('socket')
    if not sock:
        return
    try:
        sock.setsockopt(socket.SOL_SOCKET, socket.SO_KEEPALIVE, 1)
        for name, val in (('TCP_KEEPIDLE', 60), ('TCP_KEEPINTVL', 20), ('TCP_KEEPCNT', 4)):
            if hasattr(socket, name):
                sock.setsockopt(socket.IPPROTO_TCP, getattr(socket, name), val)
    except OSError:
        pass


async def pump(reader, writer, label, cid, args, log, burst):
    """Копирует reader -> writer без изменений. Возвращает причину завершения."""
    why = 'eof'
    try:
        while True:
            data = await reader.read(CHUNK)
            if not data:
                break
            log.line('#%d %s len=%d %s%s' % (cid, label, len(data), hex_head(data, args.max_hex),
                                             guess_header(data) if args.heuristic else ''))
            if burst is not None:
                burst.add(len(data))
            writer.write(data)
            await writer.drain()  # backpressure: не читаем дальше, пока не ушло
    except (ConnectionError, OSError) as e:
        why = 'error: %s' % e
    except asyncio.CancelledError:
        why = 'cancelled'
        raise
    finally:
        # half-close: передаём EOF дальше, другое направление продолжает работать
        try:
            if why == 'eof' and writer.can_write_eof() and not writer.transport.is_closing():
                writer.write_eof()
        except Exception:
            pass
    return why


async def handle(creader, cwriter, args, log, ids, tasks):
    cid = next(ids)
    peer = cwriter.get_extra_info('peername')
    log.line('#%d CONNECT stick=%s:%s' % (cid, peer[0], peer[1]) if peer else '#%d CONNECT stick=?' % cid)
    set_keepalive(cwriter)
    uphost, uport = args.upstream.host, args.upstream.port
    uhost = uphost
    try:
        uhost = await args.upstream.get(log)
        ureader, uwriter = await asyncio.wait_for(
            asyncio.open_connection(uhost, uport), CONNECT_TIMEOUT)
    except (OSError, asyncio.TimeoutError) as e:
        log.line('#%d UPSTREAM-FAIL %s:%d: %s -> closing stick' % (cid, uhost, uport, repr(e)))
        cwriter.close()
        return
    set_keepalive(uwriter)
    log.line('#%d UPSTREAM-OK %s:%d' % (cid, uhost, uport))
    burst = Burst(log, args.burst_gap)
    t_up = asyncio.ensure_future(pump(creader, uwriter, 'C->S', cid, args, log, None))
    t_dn = asyncio.ensure_future(pump(ureader, cwriter, 'S->C', cid, args, log, burst))
    tasks.update((t_up, t_dn))
    try:
        # ждём оба направления; если одно упало с ошибкой - закрываем всё
        pending = {t_up, t_dn}
        reasons = {}
        while pending:
            done, pending = await asyncio.wait(pending, return_when=asyncio.FIRST_COMPLETED)
            for t in done:
                name = 'C->S' if t is t_up else 'S->C'
                try:
                    reasons[name] = t.result()
                except asyncio.CancelledError:
                    reasons[name] = 'cancelled'
                except Exception as e:
                    reasons[name] = 'exception: %r' % e
                if not reasons[name] == 'eof':
                    for p in pending:
                        p.cancel()
                    for p in pending:
                        try:
                            await p
                        except BaseException:
                            pass
                    pending = set()
        log.line('#%d DISCONNECT stick=%s reasons=%s' % (cid, peer, reasons))
    finally:
        burst.close()
        tasks.discard(t_up)
        tasks.discard(t_dn)
        for t in (t_up, t_dn):
            t.cancel()
        for w in (cwriter, uwriter):
            try:
                w.close()
            except Exception:
                pass


async def run(args, log, stop_evt=None, ready=None):
    ids = itertools.count(1)
    tasks = set()
    conns = set()

    async def cb(r, w):
        t = asyncio.current_task()
        conns.add(t)
        try:
            await handle(r, w, args, log, ids, tasks)
        except asyncio.CancelledError:
            w.close()
        except Exception as e:
            log.line('handler error: %r' % e)
            w.close()
        finally:
            conns.discard(t)

    lhost, lport = args.listen
    server = await asyncio.start_server(cb, lhost, lport, limit=CHUNK)
    addrs = [s.getsockname() for s in server.sockets]
    log.line('LISTEN %s upstream=%s:%d burst_gap=%s' % (addrs, args.upstream.host, args.upstream.port, args.burst_gap))
    if ready is not None:
        ready(server.sockets[0].getsockname()[1])
    stop_evt = stop_evt or asyncio.Event()
    loop = asyncio.get_running_loop()
    for sig in (signal.SIGINT, signal.SIGTERM):
        try:
            loop.add_signal_handler(sig, stop_evt.set)
        except (NotImplementedError, RuntimeError, ValueError):
            pass  # не главный поток / Windows
    await stop_evt.wait()
    log.line('SHUTDOWN')
    server.close()
    for t in list(tasks):
        t.cancel()
    for t in list(conns):
        t.cancel()
    await asyncio.gather(*conns, return_exceptions=True)
    await server.wait_closed()


def build_parser():
    p = argparse.ArgumentParser(description='Прозрачный TCP-tap для Eybond/SmartESS даталоггера.')
    p.add_argument('--listen', default='0.0.0.0:502', type=lambda s: parse_hostport(s, '--listen', 0),
                   help='куда слушать, HOST:PORT (по умолчанию 0.0.0.0:502; порт уточняется)')
    p.add_argument('--upstream', required=True, type=lambda s: parse_hostport(s, '--upstream'),
                   help='настоящий сервер HOST:PORT: IP (8.218.126.236:502) или имя (ess.eybond.com:502) - '
                        'имя резолвится напрямую через --dns, минуя локальный резолвер')
    p.add_argument('--dns', default='8.8.8.8', type=lambda s: parse_hostport(s, '--dns', 0) if ':' in s else (s, 53),
                   help='DNS-сервер для имени upstream, HOST[:PORT] (по умолчанию 8.8.8.8)')
    p.add_argument('--log', default=None, help='файл лога (по умолчанию stdout)')
    p.add_argument('--max-hex', type=int, default=64, help='байт payload в hex на чанк (64)')
    p.add_argument('--burst-gap', type=float, default=3.0, help='пауза S->C, начинающая новый burst, с (3.0)')
    p.add_argument('--heuristic', action='store_true',
                   help='добавлять эвристический разбор первых 8 байт (непроверенный, только для наблюдения)')
    return p


def main(argv=None):
    args = build_parser().parse_args(argv)
    args.upstream = Upstream(args.upstream[0], args.upstream[1], args.dns[0], args.dns[1])
    log = Log(args.log)
    try:
        asyncio.run(run(args, log))
    finally:
        log.close()


if __name__ == '__main__':
    main()
