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

=== BATTERY GUARD (необязательно): последний рубеж защиты по низкому заряду ===
Независимый от остальной автоматики пассивный наблюдатель. Облако SmartESS опрашивает
инвертор (MUST PV1800, Modbus slave 4) через стик; внутри TCP-потока лежат сырые Modbus RTU
кадры (обёртка Eybond НЕ проверена и НЕ используется). Guard ищет кадры сканированием CRC16
в каждом направлении (скользящий буфер ~600 байт на соединение+направление):
  запрос    S->C: 04 03|04 addrH addrL cntH cntL crcL crcH
  ответ     C->S: 04 fn byteCount данные... crcL crcH, byteCount == 2*count свежего запроса
Из блока 25201 (count>=8) берётся напряжение АКБ (рег. 25205, x0.1 В), сеть (рег. 25207),
нагрузка (25215), реле сети (25238); из блока 20101 - рег. 20105 (ongrid). Напряжение вне
20..70 В считается мусором. Guard ничего не отправляет в стик/облако и никогда не влияет на
проксирование: все его вызовы обёрнуты в try/except, обработчик уведомлений работает в потоке.

Включение: --config /etc/eybond-tap.conf (guard включён по умолчанию, когда конфиг задан;
"guard": false выключает; без конфига guard выключен). --guard-config - устаревший синоним --config.
  --guard-dry-run     только логировать 'GUARD-DRYRUN tag=.. text=..', обработчик не вызывать
  --guard-test TAG    отправить одно тестовое событие через обработчик и выйти
Ключи guard в конфиге (все необязательны, см. etc/eybond-tap.conf.example):
  enabled=true  warn_v=51.5  alert_v=49.0  clear_v=52.5  (В; WARN при <=warn_v, ALERT при <=alert_v,
                INFO 'battery recovered' при >=clear_v после любого отправленного низкого состояния)
  consecutive=2           сколько подряд замеров ниже порога нужно для уведомления
  cooldown_warn_s=3600    повтор WARN не чаще; cooldown_alert_s=900 - повтор ALERT не чаще
                          (эскалация WARN->ALERT уходит сразу, понижение ALERT->WARN не повторяется)
  stale_s=1200            при живом соединении стика нет замеров АКБ столько секунд -> WARN 'no battery data'
  offline_s=1800          стик ни разу не подключён столько секунд (после первого подключения) -> WARN 'stick offline'
  host                    имя хоста в событии (по умолчанию /etc/hostname, иначе 'tap')
  handler                 "builtin:webhook" (по умолчанию) | "/path/to/file.py:func" | "module.name:func"
                          сигнатура: func(tag, text, value, flags, cfg) -> str; при сбое бросать исключение;
                          cfg - весь словарь конфига (плагин может читать свои ключи, напр. "handler_args")
  alert_webhook_url       URL уведомления для встроенного обработчика (любой http/https-сервис: Telegram-бот,
                          Home Assistant, ntfy, свой reg_event.php и т.п.). Плейсхолдеры подставляются в URL
                          с URL-кодированием значений:
                            ${host_name}      имя хоста (ключ host)
                            ${event_message}  текст события
                            ${event_value}    числовое значение (напряжение, В)
                            ${event_tag}      ALERT | WARN | INFO
                            ${event_flags}    флаги (целое, сейчас 0)
                            ${event_time}     время события UTC, YYYY-MM-DDTHH:MM:SSZ
                          пример: http://srv/reg_event.php?tag=${event_tag}&host=${host_name}&value=${event_value}&event=${event_message}
  alert_webhook_method    "GET" (по умолчанию) | "POST"
  alert_webhook_body      только для POST: шаблон тела (те же плейсхолдеры, значения URL-кодируются)
  alert_webhook_content_type  заголовок Content-Type для POST (по умолчанию application/x-www-form-urlencoded)
  alert_webhook_error_text    если ответ содержит эту подстроку (напр. "#ERROR") - считать сбоем (по умолчанию не задано);
                          HTTP-статус >= 400 и сетевые ошибки - сбой всегда
  alert_webhook_timeout_s таймаут запроса, с (10)
Секреты в URL (токены) в лог не попадают: пишется только схема://хост/путь без параметров.
В логе: 'GUARD sample ...' (не чаще раза в 60 с - сверить декодирование с реальностью),
'GUARD notify tag=.. text=.. -> result', 'GUARD loaded handler ...', 'GUARD-ERROR ...'.

=== ПОЛНЫЙ КОНФИГ-ФАЙЛ: --config PATH (JSON) ===
Все параметры можно задать в файле, командная строка не нужна. Приоритет:
явный флаг CLI > конфиг-файл > встроенное умолчание. Неизвестные ключи -> 'CONFIG unknown key: ..'
(предупреждение, не ошибка; ключи сохраняются для плагинов-обработчиков). Ошибки типов/диапазонов,
битый JSON, отсутствие файла -> понятное сообщение и код выхода != 0 (без traceback).
В начале работы в лог пишется 'CONFIG loaded <path> (N keys)'. Значение null = "не задано".
Ключи (умолчание в скобках):
  listen "0.0.0.0:502"        куда слушать HOST:PORT                              (флаг --listen)
  upstream null               настоящий сервер HOST:PORT, IP или имя; обязателен: в конфиге или --upstream
  dns "8.8.8.8"               DNS для имени upstream, "HOST" или "HOST:PORT"      (флаг --dns)
  log null                    файл лога (null - stdout)                           (флаг --log)
  max_hex 64                  байт payload в hex на чанк, >= 0                    (флаг --max-hex)
  burst_gap 3.0               пауза S->C, начинающая новый BURST, с, > 0          (флаг --burst-gap)
  heuristic false             эвристический разбор первых 8 байт                  (флаг --heuristic)
  guard true                  включить battery guard (false - выключить; синоним старого "enabled")
  guard_dry_run false         guard: не вызывать обработчик, только лог           (флаг --guard-dry-run)
  warn_v 51.5  alert_v 49.0  clear_v 52.5   пороги, В; требуется alert_v < warn_v < clear_v
  consecutive 2               замеров подряд ниже порога для уведомления, целое >= 1
  cooldown_warn_s 3600  cooldown_alert_s 900  stale_s 1200  offline_s 1800   секунды (см. выше)
  host null                   имя хоста в событии (null - /etc/hostname)
  handler "builtin:webhook"   обработчик уведомлений (см. выше)
  alert_webhook_url null  alert_webhook_method "GET"  alert_webhook_body null  alert_webhook_content_type  alert_webhook_error_text  alert_webhook_timeout_s 10
  handler_args null           произвольное значение для плагина (cfg['handler_args'])
  tracker true                трекер ритма опроса (см. ниже); работает и при guard=false
  cycle_gap_s 10              пауза между запросами, закрывающая цикл опроса, с, > 0
  state_file null             путь JSON-файла состояния трекера (запись атомарно tmp+rename)

=== POLL-CYCLE TRACKER: ритм "свежих данных" инвертора ===
Облако каждые ~90 с (82..97 с) опрашивает инвертор полным циклом: ~10 Modbus-запросов (slave 4)
с шагом ~0.3 с в течение ~5.5 с, каждый получает ответ стика с живыми данными. Отдельный
не-Modbus heartbeat (~36 с, 16 Б/26 Б) циклом НЕ считается: трекер питается только запросами/
ответами, найденными CRC-сканом guard (а не размерами чанков). Цикл = группа запросов с паузами
< cycle_gap_s; закрывается, когда запросов нет cycle_gap_s. На закрытие пишется строка:
  CYCLE start=<UTC iso> dur=<с> n_req=<n> blocks=<25201x74,...> fresh=<k>/<n> pack_v=<В|?>
        since_prev=<с|?> period=<медиана|?> locked=<true|false>
fresh - сколько запросов цикла получили сопоставленный ответ; pack_v - из блока 25201 цикла;
period - медиана до 8 последних интервалов между стартами циклов; locked - не менее 3 интервалов
в пределах +-15% от медианы. Хранятся последние 64 цикла. Если задан state_file, при каждом
закрытии цикла атомарно пишется JSON: updated_utc, last_start_utc, last_start_epoch, period_s,
jitter_s (max-min интервалов), locked, next_start_epoch (старт + период), last_pack_v,
last_sample_age_s, cycles_seen. Ошибки трекера/файла логируются как 'TRACKER-ERROR ...' и
на проксирование не влияют.
"""
import argparse
import asyncio
import datetime
import functools
import importlib
import importlib.util
import ipaddress
import itertools
import json
import math
import os
import re
import statistics
import collections
import urllib.parse
import urllib.error
import urllib.request
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
        self.fh = (ensure_parent_dir(path) or open(path, 'a', encoding='utf-8')) if path else sys.stdout

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


# ======================= BATTERY GUARD =======================
# Чистая логика (Guard) без I/O и без asyncio; время передаётся снаружи (time.monotonic()).

GUARD_SLAVE = 4              # Modbus slave id инвертора
GUARD_BUF = 600              # размер скользящего буфера на соединение+направление, байт
GUARD_REQ_KEEP = 8           # сколько последних запросов помнить на соединение
GUARD_REQ_TTL = 30.0         # время жизни запроса, с
GUARD_V_MIN, GUARD_V_MAX = 20.0, 70.0   # фильтр правдоподобия напряжения АКБ, В
GUARD_DEFAULTS = {
    'enabled': True, 'warn_v': 51.5, 'alert_v': 49.0, 'clear_v': 52.5, 'consecutive': 2,
    'cooldown_warn_s': 3600, 'cooldown_alert_s': 900, 'stale_s': 1200, 'offline_s': 1800,
    'host': None, 'handler': 'builtin:webhook',
    'alert_webhook_url': None, 'alert_webhook_method': 'GET', 'alert_webhook_body': None,
    'alert_webhook_content_type': 'application/x-www-form-urlencoded', 'alert_webhook_error_text': None,
    'alert_webhook_timeout_s': 10,
}
WEBHOOK_PLACEHOLDERS = ('host_name', 'event_message', 'event_value', 'event_tag', 'event_flags', 'event_time')
_PH_RE = re.compile(r'\$\{([A-Za-z0-9_]*)\}')


def modbus_crc(data):
    """CRC16 Modbus: init 0xFFFF, poly 0xA001; в кадре идёт младшим байтом вперёд."""
    crc = 0xFFFF
    for b in data:
        crc ^= b
        for _ in range(8):
            crc = (crc >> 1) ^ 0xA001 if crc & 1 else crc >> 1
    return crc


def crc_ok(frame):
    return len(frame) >= 4 and modbus_crc(frame[:-2]) == frame[-2] | (frame[-1] << 8)


def mk_frame(payload):
    """payload + CRC (для тестов и построения запросов)."""
    c = modbus_crc(payload)
    return bytes(payload) + bytes((c & 0xFF, c >> 8))


def _dec_rt(r, s):      # блок 25201: рег. N -> r[N-25201]
    s['pack_v'] = r[4] * 0.1
    if len(r) >= 7:
        s['grid_v'] = r[6] * 0.1
    if len(r) >= 15:
        s['load_w'] = r[14]
    if len(r) >= 38:
        s['relay_grid'] = 1 if r[37] == 1 else 0


def _dec_cfg(r, s):     # блок 20101: рег. 20105
    s['ongrid_reg'] = r[4]


# таблица известных блоков: start -> (минимальный count, декодер); новые блоки добавлять сюда
GUARD_BLOCKS = {
    25201: (8, _dec_rt),
    20101: (5, _dec_cfg),
}


class _GConn:
    def __init__(self):
        self.buf = {'S->C': bytearray(), 'C->S': bytearray()}
        self.reqs = []   # [(t, fn, addr, count)]


class Guard:
    """Пассивный разбор Modbus во проксируемом трафике + конечный автомат уведомлений.
    Методы возвращают список событий (tag, text, value, flags); ничего не отправляют сами."""

    def __init__(self, cfg=None, notify=True):
        self.notify = notify          # False: только декодирование (для трекера), события не выдаются
        self.observer = None          # объект с on_request(now, addr, cnt) / on_response(now, addr, regs)
        c = dict(GUARD_DEFAULTS)
        c.update(cfg or {})
        self.cfg = c
        self.enabled = bool(c['enabled'])
        self.conns = {}
        self.last_sample_t = None
        self.last_sample = None       # dict последнего правдоподобного замера
        self.sample_seq = 0
        self.grid = {}                # последнее известное relay_grid / ongrid_reg
        self.load_w = None
        self.level = 0                # текущий уровень по последнему замеру: 0 ok / 1 warn / 2 alert
        self.streak_w = 0             # подряд замеров уровня >=1
        self.streak_a = 0             # подряд замеров уровня 2
        self.notified_low = False
        self.last_warn_t = None
        self.last_alert_t = None
        self.last_stale_t = None
        self.last_offline_t = None
        self.last_conn_open = None
        self.last_conn_close = None
        self.seen_conn = False

    # --- соединения ---
    def conn_open(self, now, cid):
        self.conns[cid] = _GConn()
        self.seen_conn = True
        self.last_conn_open = now

    def conn_close(self, now, cid):
        self.conns.pop(cid, None)          # буферы соединения выбрасываем
        self.last_conn_close = now

    # --- разбор потока ---
    def feed(self, now, cid, direction, data):
        """Скармливает чанк; возвращает список событий. Не бросает исключений наружу."""
        if not self.enabled:
            return []
        try:
            return self._feed(now, cid, direction, data)
        except Exception:
            return []

    def _feed(self, now, cid, direction, data):
        conn = self.conns.get(cid)
        if conn is None:
            conn = self.conns[cid] = _GConn()
        buf = conn.buf[direction]
        buf += data
        events = []
        if direction == 'S->C':
            self._scan_requests(now, conn, buf)
        else:
            for regs_start, regs in self._scan_responses(now, conn, buf):
                self._observe('on_response', now, regs_start, regs)
                events += self._on_block(now, regs_start, regs)
        if len(buf) > GUARD_BUF:
            del buf[:len(buf) - GUARD_BUF]
        return events if self.notify else []

    def _observe(self, name, *a):
        obs = self.observer
        if obs is not None:
            try:
                getattr(obs, name)(*a)
            except Exception:
                pass          # наблюдатель не должен мешать разбору

    def _scan_requests(self, now, conn, buf):
        last_end = 0
        s = 0
        while True:
            s = buf.find(GUARD_SLAVE, s)
            if s < 0 or s + 8 > len(buf):
                break
            if buf[s + 1] in (3, 4) and crc_ok(bytes(buf[s:s + 8])):
                addr = (buf[s + 2] << 8) | buf[s + 3]
                cnt = (buf[s + 4] << 8) | buf[s + 5]
                if 1 <= cnt <= 125:
                    conn.reqs.append((now, buf[s + 1], addr, cnt))
                    del conn.reqs[:-GUARD_REQ_KEEP]
                    self._observe('on_request', now, addr, cnt)
                    s += 8
                    last_end = s
                    continue
            s += 1
        if last_end:
            del buf[:last_end]

    @staticmethod
    def _scan_responses(now, conn, buf):
        conn.reqs[:] = [r for r in conn.reqs if now - r[0] <= GUARD_REQ_TTL]
        found = []
        last_end = 0
        s = 0
        if not conn.reqs:
            del buf[:]    # без ожидающего запроса ответ не опознать; старьё не копим (иначе заматчится на чужой запрос)
        while conn.reqs:
            s = buf.find(GUARD_SLAVE, s)
            if s < 0 or s + 5 > len(buf):
                break
            fn, bc = buf[s + 1], buf[s + 2]
            if fn in (3, 4) and bc % 2 == 0 and bc:
                end = s + 3 + bc + 2
                req = next((r for r in reversed(conn.reqs) if r[1] == fn and 2 * r[3] == bc), None)
                if req is not None and end <= len(buf) and crc_ok(bytes(buf[s:end])):
                    n = bc // 2
                    found.append((req[2], struct.unpack('>%dH' % n, bytes(buf[s + 3:s + 3 + bc]))))
                    conn.reqs.remove(req)
                    s = last_end = end
                    continue
            s += 1
        if last_end:
            del buf[:last_end]
        return found

    # --- декодирование блока и состояние ---
    def _on_block(self, now, start, regs):
        spec = GUARD_BLOCKS.get(start)
        if not spec or len(regs) < spec[0]:
            return []
        smp = {}
        spec[1](regs, smp)
        for k in ('relay_grid', 'ongrid_reg'):
            if k in smp:
                self.grid[k] = smp[k]
        if 'load_w' in smp:
            self.load_w = smp['load_w']
        pv = smp.get('pack_v')
        if pv is None or not GUARD_V_MIN <= pv <= GUARD_V_MAX:
            return []
        smp['t'] = now
        self.last_sample, self.last_sample_t = smp, now
        self.sample_seq += 1
        return self.on_sample(now, pv)

    def grid_text(self):
        g = self.grid.get('relay_grid')
        if g is None:
            g = self.grid.get('ongrid_reg')
        if g is None:
            return None
        return 'сеть подключена' if g == 1 else 'работа от батареи'

    def _details(self):
        d = []
        gt = self.grid_text()
        if gt:
            d.append(gt)
        if self.load_w is not None:
            d.append('нагрузка %d Вт' % self.load_w)
        return (', ' + ', '.join(d)) if d else ''

    def on_sample(self, now, pv):
        c = self.cfg
        lvl = 2 if pv <= c['alert_v'] else 1 if pv <= c['warn_v'] else 0
        self.level = lvl
        self.streak_w = self.streak_w + 1 if lvl >= 1 else 0
        self.streak_a = self.streak_a + 1 if lvl == 2 else 0
        ev = []
        need = max(1, int(c['consecutive']))
        if lvl == 2 and self.streak_a >= need:
            if self.last_alert_t is None or now - self.last_alert_t >= c['cooldown_alert_s']:
                self.last_alert_t = self.last_warn_t = now     # понижение до WARN не должно сразу переуведомлять
                self.notified_low = True
                ev.append(('ALERT', 'Батарея: КРИТИЧЕСКИ низкое напряжение %.1f В (порог %.1f В)%s'
                           % (pv, c['alert_v'], self._details()), pv, 0))
        elif lvl == 1 and self.streak_w >= need:
            if self.last_warn_t is None or now - self.last_warn_t >= c['cooldown_warn_s']:
                self.last_warn_t = now
                self.notified_low = True
                ev.append(('WARN', 'Батарея: низкое напряжение %.1f В (порог %.1f В)%s'
                           % (pv, c['warn_v'], self._details()), pv, 0))
        elif pv >= c['clear_v'] and self.notified_low:
            self.notified_low = False
            self.last_warn_t = self.last_alert_t = None
            ev.append(('INFO', 'Батарея: напряжение восстановлено %.1f В (battery recovered)%s'
                       % (pv, self._details()), pv, 0))
        return ev

    def tick(self, now):
        """Периодические проверки: нет данных / стик offline. Возвращает события."""
        if not self.enabled or not self.notify:
            return []
        try:
            return self._tick(now)
        except Exception:
            return []

    def _tick(self, now):
        c, ev = self.cfg, []
        if self.conns:
            ref = max(t for t in (self.last_sample_t, self.last_conn_open) if t is not None)
            if now - ref >= c['stale_s'] and (self.last_stale_t is None
                                              or now - self.last_stale_t >= c['cooldown_warn_s']):
                self.last_stale_t = now
                ev.append(('WARN', 'Батарея: нет данных о напряжении (no battery data) уже %d мин при живом соединении стика'
                           % ((now - ref) // 60), 0.0, 0))
        elif self.seen_conn and self.last_conn_close is not None:
            if now - self.last_conn_close >= c['offline_s'] and (
                    self.last_offline_t is None or now - self.last_offline_t >= c['cooldown_warn_s']):
                self.last_offline_t = now
                ev.append(('WARN', 'Стик не подключён (stick offline) уже %d мин, батарея не контролируется'
                           % ((now - self.last_conn_close) // 60), 0.0, 0))
        return ev


# ======================= POLL-CYCLE TRACKER =======================
# Чистая логика без I/O и asyncio: время передаётся снаружи (now = monotonic), часы инъектируются.

def _iso_utc(epoch):
    return datetime.datetime.fromtimestamp(epoch, datetime.timezone.utc).strftime('%Y-%m-%dT%H:%M:%SZ')


class CycleTracker:
    """Ритм опроса инвертора облаком. Цикл = группа Modbus-запросов с паузами < gap_s.
    request()/response()/tick() возвращают словарь закрытого цикла (или None); сам ничего не пишет."""

    KEEP = 64            # сколько последних циклов помнить
    PERIOD_N = 8         # по скольким последним интервалам считать период
    LOCK_TOL = 0.15      # допуск "захвата" ритма, доля от медианы
    LOCK_MIN = 3         # минимум интервалов в допуске для locked
    PACK_BLOCK = 25201   # блок, из которого берём напряжение АКБ

    def __init__(self, gap_s=10.0, clock=time.monotonic, wall=time.time):
        self.gap = float(gap_s)
        self.clock = clock
        self._off = wall() - clock()      # перевод monotonic -> epoch
        self.cur = None
        self.cycles = collections.deque(maxlen=self.KEEP)
        self.cycles_seen = 0
        self.last_pack_v = None
        self.last_pack_t = None

    def epoch(self, now):
        return self._off + now

    def request(self, now, addr, cnt):
        closed = None
        if self.cur is not None and now - self.cur['last_req'] >= self.gap:
            closed = self._close()
        if self.cur is None:
            self.cur = {'start': now, 'start_epoch': self.epoch(now), 'last_req': now, 'last': now,
                        'blocks': [], 'pack_v': None}
        c = self.cur
        c['blocks'].append({'addr': addr, 'cnt': cnt, 'fresh': False})
        c['last_req'] = c['last'] = now
        return closed

    def response(self, now, addr, regs):
        pv = None
        if addr == self.PACK_BLOCK and len(regs) >= 5:
            v = regs[4] * 0.1
            if GUARD_V_MIN <= v <= GUARD_V_MAX:
                pv = v
                self.last_pack_v, self.last_pack_t = v, now
        c = self.cur
        if c is None:
            return None
        for b in reversed(c['blocks']):
            if not b['fresh'] and b['addr'] == addr and b['cnt'] == len(regs):
                b['fresh'] = True
                c['last'] = max(c['last'], now)
                if pv is not None:
                    c['pack_v'] = pv
                break
        return None

    def tick(self, now):
        if self.cur is not None and now - self.cur['last_req'] >= self.gap:
            return self._close()
        return None

    def _intervals(self):
        st = [c['start'] for c in self.cycles]
        return [b - a for a, b in zip(st, st[1:])][-self.PERIOD_N:]

    def stats(self):
        """(period, jitter, locked) по последним интервалам; period/jitter = None, если интервалов нет."""
        iv = self._intervals()
        if not iv:
            return None, None, False
        med = statistics.median(iv)
        ok = sum(1 for x in iv if med > 0 and abs(x - med) <= self.LOCK_TOL * med)
        return med, max(iv) - min(iv), ok >= self.LOCK_MIN

    def _close(self):
        c, self.cur = self.cur, None
        prev = self.cycles[-1]['start'] if self.cycles else None
        rec = {'start': c['start'], 'start_epoch': c['start_epoch'], 'dur': c['last'] - c['start'],
               'n_req': len(c['blocks']), 'blocks': [(b['addr'], b['cnt']) for b in c['blocks']],
               'fresh': sum(1 for b in c['blocks'] if b['fresh']), 'pack_v': c['pack_v'],
               'since_prev': (c['start'] - prev) if prev is not None else None}
        self.cycles.append(rec)
        self.cycles_seen += 1
        rec['period'], rec['jitter'], rec['locked'] = self.stats()
        return rec

    @staticmethod
    def format(rec):
        return ('CYCLE start=%s dur=%.1f n_req=%d blocks=%s fresh=%d/%d pack_v=%s since_prev=%s period=%s locked=%s'
                % (_iso_utc(rec['start_epoch']), rec['dur'], rec['n_req'],
                   ','.join('%dx%d' % b for b in rec['blocks']), rec['fresh'], rec['n_req'],
                   '%.1f' % rec['pack_v'] if rec['pack_v'] is not None else '?',
                   '%.1f' % rec['since_prev'] if rec['since_prev'] is not None else '?',
                   '%.1f' % rec['period'] if rec['period'] is not None else '?',
                   'true' if rec['locked'] else 'false'))

    def state(self, now):
        """Содержимое state-файла (dict, готов к json.dump)."""
        period, jitter, locked = self.stats()
        last = self.cycles[-1] if self.cycles else None
        return {
            'updated_utc': _iso_utc(self.epoch(now)),
            'last_start_utc': _iso_utc(last['start_epoch']) if last else None,
            'last_start_epoch': last['start_epoch'] if last else None,
            'period_s': period, 'jitter_s': jitter, 'locked': locked,
            'next_start_epoch': (last['start_epoch'] + period) if last and period is not None else None,
            'last_pack_v': self.last_pack_v,
            'last_sample_age_s': (now - self.last_pack_t) if self.last_pack_t is not None else None,
            'cycles_seen': self.cycles_seen,
        }


def ensure_parent_dir(path):
    """Создаёт каталог файла, если его нет (повторные вызовы дёшевы)."""
    d = os.path.dirname(os.path.abspath(path))
    if d:
        os.makedirs(d, exist_ok=True)


def write_state_file(path, state):
    """Атомарная запись JSON (tmp + rename)."""
    ensure_parent_dir(path)
    tmp = '%s.tmp%d' % (path, os.getpid())
    with open(tmp, 'w', encoding='utf-8') as f:
        json.dump(state, f, ensure_ascii=False)
        f.write('\n')
    os.replace(tmp, path)


# --- конфиг и обработчики уведомлений ---

def guard_host(cfg):
    if cfg.get('host'):
        return str(cfg['host'])
    try:
        with open('/etc/hostname', encoding='utf-8') as f:
            h = f.read().strip()
        if h:
            return h
    except OSError:
        pass
    return 'tap'


class ConfigError(ValueError):
    """Ошибка конфиг-файла/параметров: сообщение показывается пользователю без traceback."""


def parse_dns(s):
    return parse_hostport(s, 'dns', 0) if ':' in s else (s, 53)


# умолчания всех параметров (CLI > файл > эти значения)
CONFIG_DEFAULTS = dict(GUARD_DEFAULTS, **{
    'listen': ('0.0.0.0', 502), 'upstream': None, 'dns': ('8.8.8.8', 53), 'log': None,
    'max_hex': 64, 'burst_gap': 3.0, 'heuristic': False, 'guard': True, 'guard_dry_run': False,
    'handler_args': None, 'tracker': True, 'cycle_gap_s': 10.0, 'state_file': None,
    'regs_file': None,
})
# ключи, принимающие null (= не задано)
CONFIG_NULLABLE = {'upstream', 'log', 'host', 'handler_args', 'state_file', 'regs_file', 'alert_webhook_url', 'alert_webhook_body',
                   'alert_webhook_error_text'}
# 'enabled' - прежнее имя ключа 'guard' (guard имеет приоритет)
CONFIG_KNOWN = set(CONFIG_DEFAULTS) | {'enabled'}


def _num(key, v, lo=None, hi=None, strict_lo=False, integer=False):
    if isinstance(v, bool) or not isinstance(v, (int, float)) or not math.isfinite(v):
        raise ConfigError('%s: ожидается %s, получено %r' % (key, 'целое число' if integer else 'число', v))
    if integer and (not isinstance(v, int)):
        if float(v) != int(v):
            raise ConfigError('%s: ожидается целое число, получено %r' % (key, v))
        v = int(v)
    if lo is not None and (v <= lo if strict_lo else v < lo):
        raise ConfigError('%s: должно быть %s %s, получено %r' % (key, '>' if strict_lo else '>=', lo, v))
    if hi is not None and v > hi:
        raise ConfigError('%s: должно быть <= %s, получено %r' % (key, hi, v))
    return v


def _hostport(key, v, min_port):
    if not isinstance(v, str):
        raise ConfigError('%s: ожидается строка HOST:PORT, получено %r' % (key, v))
    try:
        return parse_hostport(v, key, min_port)
    except argparse.ArgumentTypeError as e:
        raise ConfigError(str(e))


def validate_config(user):
    """Проверяет dict из JSON. Возвращает (нормализованный dict, список неизвестных ключей).
    Неизвестные ключи остаются в dict как есть (для плагинов). Бросает ConfigError."""
    if not isinstance(user, dict):
        raise ConfigError('конфиг должен быть JSON-объектом')
    out, unknown = {}, []
    for k, v in user.items():
        if k not in CONFIG_KNOWN:
            unknown.append(k)
            out[k] = v
            continue
        if v is None:
            if k in CONFIG_NULLABLE:
                continue                       # null = ключ не задан
            raise ConfigError('%s: null недопустим' % k)
        if k == 'listen':
            v = _hostport(k, v, 0)
        elif k == 'upstream':
            v = _hostport(k, v, 1)
        elif k == 'dns':
            if not isinstance(v, str) or not v:
                raise ConfigError('dns: ожидается строка "HOST" или "HOST:PORT", получено %r' % (v,))
            v = _hostport(k, v, 0) if ':' in v else (v, 53)
        elif k in ('log', 'host', 'handler', 'state_file', 'regs_file', 'alert_webhook_content_type', 'alert_webhook_error_text'):
            if not isinstance(v, str) or not v:
                raise ConfigError('%s: ожидается непустая строка, получено %r' % (k, v))
        elif k in ('alert_webhook_url', 'alert_webhook_body'):
            if not isinstance(v, str) or not v:
                raise ConfigError('%s: ожидается непустая строка, получено %r' % (k, v))
            if k == 'alert_webhook_url' and not re.match(r'^https?://[^\s/]+', v):
                raise ConfigError('alert_webhook_url: ожидается http:// или https:// URL, получено %r' % (v,))
            bad = sorted({n for n in _PH_RE.findall(v) if n not in WEBHOOK_PLACEHOLDERS})
            if bad:
                raise ConfigError('%s: неизвестные плейсхолдеры %s; допустимы: %s' % (
                    k, ', '.join('${%s}' % n for n in bad), ', '.join('${%s}' % n for n in WEBHOOK_PLACEHOLDERS)))
        elif k == 'alert_webhook_method':
            if not isinstance(v, str) or v.upper() not in ('GET', 'POST'):
                raise ConfigError('alert_webhook_method: ожидается "GET" или "POST", получено %r' % (v,))
            v = v.upper()
        elif k == 'alert_webhook_timeout_s':
            v = _num(k, v, 0, 120, strict_lo=True)
        elif k in ('heuristic', 'guard', 'enabled', 'guard_dry_run', 'tracker'):
            if not isinstance(v, bool):
                raise ConfigError('%s: ожидается true/false, получено %r' % (k, v))
        elif k == 'max_hex':
            v = _num(k, v, 0, integer=True)
        elif k == 'consecutive':
            v = _num(k, v, 1, integer=True)
        elif k in ('burst_gap', 'cycle_gap_s', 'stale_s', 'offline_s'):
            v = _num(k, v, 0, strict_lo=True)
        elif k in ('cooldown_warn_s', 'cooldown_alert_s'):
            v = _num(k, v, 0)
        elif k in ('warn_v', 'alert_v', 'clear_v'):
            v = _num(k, v, 0, 100, strict_lo=True)
        out[k] = v
    return out, unknown


def check_thresholds(cfg):
    if not cfg['alert_v'] < cfg['warn_v']:
        raise ConfigError('warn_v (%s) должен быть больше alert_v (%s)' % (cfg['warn_v'], cfg['alert_v']))
    if not cfg['warn_v'] < cfg['clear_v']:
        raise ConfigError('clear_v (%s) должен быть больше warn_v (%s)' % (cfg['clear_v'], cfg['warn_v']))


def load_config_file(path):
    """Читает и проверяет JSON-конфиг -> (dict, неизвестные ключи, число ключей). Бросает ConfigError."""
    try:
        with open(path, encoding='utf-8') as f:
            user = json.load(f)
    except OSError as e:
        raise ConfigError('не удалось прочитать файл: %s' % (e.strerror or e))
    except ValueError as e:
        raise ConfigError('некорректный JSON: %s' % e)
    if isinstance(user, dict) and 'alert_webook_url' in user and 'alert_webhook_url' not in user:
        user = dict(user)
        user['alert_webhook_url'] = user.pop('alert_webook_url')       # частая опечатка в имени ключа
    cfg, unknown = validate_config(user)
    return cfg, unknown, len(user)


def load_guard_config(path):
    """JSON-конфиг поверх умолчаний guard; неизвестные ключи сохраняются (для плагинов). Бросает при ошибке."""
    user, _unknown, _n = load_config_file(path)
    cfg = dict(GUARD_DEFAULTS)
    cfg.update(user)
    check_thresholds(cfg)
    cfg['host'] = guard_host(cfg)
    return cfg


def resolve_settings(parser, argv):
    """Разбор CLI + конфиг-файл. Приоритет: явный флаг CLI > файл > умолчание.
    Возвращает args с заполненными полями и args.settings (полный dict). Ошибки -> parser.error (exit 2)."""
    args = parser.parse_args(argv)
    explicit = vars(build_parser(False).parse_args(argv))     # только реально указанные флаги
    path = args.config or args.guard_config
    msgs = []
    if args.config and args.guard_config:
        msgs.append('CONFIG --guard-config ignored (deprecated, --config given)')
    elif args.guard_config:
        msgs.append('CONFIG --guard-config is deprecated, use --config')
    fcfg, nkeys = {}, 0
    if path:
        try:
            fcfg, unknown, nkeys = load_config_file(path)
        except ConfigError as e:
            parser.error('конфиг %s: %s' % (path, e))
        msgs.append('CONFIG loaded %s (%d keys)' % (path, nkeys))
        msgs += ['CONFIG unknown key: %s' % k for k in unknown]
    s = dict(CONFIG_DEFAULTS)
    s.update(fcfg)
    s['guard'] = fcfg.get('guard', fcfg.get('enabled', bool(path)))
    for k in ('listen', 'upstream', 'dns', 'log', 'max_hex', 'burst_gap', 'heuristic'):
        if explicit.get(k) is not None:        # upstream имеет default=None и попадает в explicit всегда
            s[k] = explicit[k]
    if explicit.get('guard_dry_run'):
        s['guard_dry_run'] = True
    s['enabled'] = s['guard']
    try:
        check_thresholds(s)
    except ConfigError as e:
        parser.error('конфиг %s: %s' % (path, e))
    s['host'] = guard_host(s)
    args.settings, args.config_msgs, args.config_path = s, msgs, path
    args.guard_config = path       # совместимость: guard_self_test / run
    return args


def render_template(tpl, values):
    """Подставляет ${имя} из values с URL-кодированием значений; неизвестное имя -> ValueError."""
    def sub(m):
        name = m.group(1)
        if name not in values:
            raise ValueError('unknown placeholder ${%s}' % name)
        return urllib.parse.quote(str(values[name]), safe='')
    return _PH_RE.sub(sub, tpl)


def _fmt_value(v):
    try:
        return ('%.3f' % float(v)).rstrip('0').rstrip('.')
    except (TypeError, ValueError):
        return str(v)


def safe_url_for_log(url):
    """схема://хост/путь без параметров запроса (в них могут быть токены)."""
    p = urllib.parse.urlsplit(url)
    return '%s://%s%s' % (p.scheme, p.netloc.rpartition('@')[2], p.path)


def webhook_handler(tag, text, value, flags, cfg):
    """Встроенный обработчик: HTTP-запрос на alert_webhook_url с подстановкой плейсхолдеров."""
    tpl = cfg.get('alert_webhook_url')
    if not tpl:
        raise ValueError('alert_webhook_url is not set')
    values = {'host_name': guard_host(cfg), 'event_message': text, 'event_value': _fmt_value(value),
              'event_tag': tag, 'event_flags': flags,
              'event_time': datetime.datetime.now(datetime.timezone.utc).strftime('%Y-%m-%dT%H:%M:%SZ')}
    url = render_template(tpl, values)
    method = (cfg.get('alert_webhook_method') or 'GET').upper()
    data, headers = None, {}
    if method == 'POST':
        body_tpl = cfg.get('alert_webhook_body') or ''
        data = render_template(body_tpl, values).encode('utf-8')
        headers['Content-Type'] = cfg.get('alert_webhook_content_type') or 'application/x-www-form-urlencoded'
    req = urllib.request.Request(url, data=data, method=method, headers=headers)
    try:
        with urllib.request.urlopen(req, timeout=float(cfg.get('alert_webhook_timeout_s') or 10)) as r:
            resp = r.read(65536).decode('utf-8', 'replace')
    except urllib.error.HTTPError as e:
        raise RuntimeError('webhook %s %s: HTTP %s' % (method, safe_url_for_log(url), e.code))
    except urllib.error.URLError as e:
        raise RuntimeError('webhook %s %s: %s' % (method, safe_url_for_log(url), e.reason))
    marker = cfg.get('alert_webhook_error_text')
    if marker and marker in resp:
        raise RuntimeError('webhook %s: response contains %r: %s' % (safe_url_for_log(url), marker, resp.strip()[:200]))
    return ('%s %s -> %s' % (method, safe_url_for_log(url), resp.strip()[:120])).strip()


def load_handler(spec):
    """'builtin:webhook' | '/path/file.py:func' | 'module.name:func' -> (callable, описание)."""
    spec = spec or 'builtin:webhook'
    where, sep, name = spec.rpartition(':')
    if not sep or not where or not name:
        raise ValueError('handler must be "<file.py|module>:<func>", got %r' % spec)
    if where == 'builtin':
        if name != 'webhook':
            raise ValueError('unknown builtin handler %r' % name)
        return webhook_handler, spec
    if where.endswith('.py') or os.sep in where or '/' in where:
        sp = importlib.util.spec_from_file_location('eybond_guard_handler_%x' % (hash(where) & 0xffff), where)
        if sp is None or sp.loader is None:
            raise ImportError('cannot load %s' % where)
        mod = importlib.util.module_from_spec(sp)
        sp.loader.exec_module(mod)
    else:
        mod = importlib.import_module(where)
    fn = getattr(mod, name)
    if not callable(fn):
        raise TypeError('%s is not callable' % spec)
    return fn, spec


class RegCache:
    """Последние ответы инвертора по блокам (стартовый адрес -> (epoch, слова)) в JSON для pv1800_ctrl.py:
    запасной источник данных, если плата grid-supervisor недоступна (см. sbin/gs_tapregs.py).
    put() только запоминает; запись раз в секунду из тикера, чтобы не тормозить проксирование."""

    def __init__(self, path, wall=time.time):
        self.path, self.wall = path, wall
        self.blocks = {}
        self.dirty = False

    def put(self, addr, regs):
        self.blocks[addr] = (self.wall(), [int(x) for x in regs])
        self.dirty = True

    def snapshot(self):
        return {'t': max((t for t, _ in self.blocks.values()), default=0),
                'blocks': [{'addr': a, 'count': len(r), 't': t, 'regs': r} for a, (t, r) in sorted(self.blocks.items())]}

    def flush(self, force=False):
        if not self.dirty and not force:
            return False
        self.dirty = False
        write_state_file(self.path, self.snapshot())
        return True


class GuardRunner:
    """Связка Guard с asyncio: очередь событий, последовательные вызовы обработчика в потоке с
    таймаутом, периодический tick. Любая ошибка логируется и не влияет на проксирование."""

    TICK_S = 30.0
    HANDLER_TIMEOUT = 15.0
    SAMPLE_LOG_S = 60.0

    def __init__(self, cfg, log, handler=None, dry_run=False, clock=time.monotonic, notify=True, wall=time.time):
        self.cfg, self.log, self.dry_run, self.clock = cfg, log, dry_run, clock
        self.notify = notify          # False: guard только декодирует (режим "только трекер")
        gcfg = dict(cfg)
        if not notify:
            gcfg['enabled'] = True
        self.guard = Guard(gcfg, notify=notify)
        self.tracker = None
        self.state_file = cfg.get('state_file')
        self.regs = RegCache(cfg['regs_file'], wall) if cfg.get('regs_file') else None
        if cfg.get('tracker', False):            # без ключа (старые вызовы) трекер выключен
            self.tracker = CycleTracker(cfg.get('cycle_gap_s', 10.0), clock, wall)
            self.guard.observer = self
        if self.regs is not None:
            self.guard.observer = self
        self.handler = handler
        self.queue = None
        self.tasks = []
        self.last_logged_seq = 0
        self.last_log_t = None
        if not notify:
            pass
        elif handler is None and not dry_run:
            try:
                self.handler, desc = load_handler(cfg.get('handler'))
                log.line('GUARD loaded handler %s' % desc)
            except Exception as e:
                log.line('GUARD-ERROR cannot load handler %r: %r' % (cfg.get('handler'), e))
        elif dry_run:
            log.line('GUARD dry-run: handler not called')

    def start(self):
        self.queue = asyncio.Queue(maxsize=50)
        loop = asyncio.get_running_loop()
        self.tasks = [loop.create_task(self._worker()), loop.create_task(self._ticker())]
        if self.tracker is not None:
            self.tasks.append(loop.create_task(self._tracker_ticker()))
        if self.regs is not None:
            self.tasks.append(loop.create_task(self._regs_ticker()))

    async def stop(self):
        for t in self.tasks:
            t.cancel()
        await asyncio.gather(*self.tasks, return_exceptions=True)
        self.tasks = []
        if self.regs is not None:
            try:
                self.regs.flush(force=True)
            except Exception as e:
                self.log.line('REGS-ERROR final flush %s: %r' % (self.regs.path, e))

    # --- трекер ритма опроса (ошибки не влияют на проксирование) ---
    def on_request(self, now, addr, cnt):
        if self.tracker is None:
            return
        try:
            self._cycle_closed(self.tracker.request(now, addr, cnt))
        except Exception as e:
            self.log.line('TRACKER-ERROR request: %r' % e)

    def on_response(self, now, addr, regs):
        if self.regs is not None:
            self.regs.put(addr, regs)
        if self.tracker is None:
            return
        try:
            self.tracker.response(now, addr, regs)
        except Exception as e:
            self.log.line('TRACKER-ERROR response: %r' % e)

    def _cycle_closed(self, rec):
        if rec is None:
            return
        self.log.line(CycleTracker.format(rec))
        if self.state_file:
            try:
                write_state_file(self.state_file, self.tracker.state(self.clock()))
            except Exception as e:
                self.log.line('TRACKER-ERROR state_file %s: %r' % (self.state_file, e))

    async def _regs_ticker(self):
        while True:
            await asyncio.sleep(1.0)
            try:
                self.regs.flush()
            except Exception as e:
                self.log.line('REGS-ERROR flush %s: %r' % (self.regs.path, e))

    async def _tracker_ticker(self):
        step = max(0.05, min(1.0, self.tracker.gap / 3))
        while True:
            await asyncio.sleep(step)
            try:
                self._cycle_closed(self.tracker.tick(self.clock()))
            except Exception as e:
                self.log.line('TRACKER-ERROR tick: %r' % e)

    def conn_open(self, cid):
        try:
            self.guard.conn_open(self.clock(), cid)
        except Exception as e:
            self.log.line('GUARD-ERROR conn_open: %r' % e)

    def conn_close(self, cid):
        try:
            self.guard.conn_close(self.clock(), cid)
        except Exception as e:
            self.log.line('GUARD-ERROR conn_close: %r' % e)

    def feed(self, cid, direction, data):
        try:
            now = self.clock()
            self._emit(self.guard.feed(now, cid, direction, data))
            g = self.guard
            if g.sample_seq != self.last_logged_seq and (
                    self.last_log_t is None or now - self.last_log_t >= self.SAMPLE_LOG_S):
                self.last_logged_seq, self.last_log_t = g.sample_seq, now
                s = g.last_sample or {}
                self.log.line('GUARD sample pack_v=%.1f grid=%s grid_v=%s load=%s ongrid_reg=%s' % (
                    s.get('pack_v', -1), g.grid_text() or '?',
                    '%.1f' % s['grid_v'] if 'grid_v' in s else '?',
                    g.load_w if g.load_w is not None else '?', g.grid.get('ongrid_reg', '?')))
        except Exception as e:
            self.log.line('GUARD-ERROR feed: %r' % e)

    def _emit(self, events):
        for ev in events:
            if self.queue is None:
                continue
            try:
                self.queue.put_nowait(ev)
            except asyncio.QueueFull:
                self.log.line('GUARD-ERROR queue full, dropped %s' % (ev[0],))

    async def _ticker(self):
        while True:
            await asyncio.sleep(self.TICK_S)
            try:
                self._emit(self.guard.tick(self.clock()))
            except Exception as e:
                self.log.line('GUARD-ERROR tick: %r' % e)

    async def _worker(self):
        while True:
            ev = await self.queue.get()
            try:
                await self.deliver(*ev)
            except Exception as e:
                self.log.line('GUARD-ERROR deliver: %r' % e)

    async def deliver(self, tag, text, value, flags):
        """Один вызов обработчика (в потоке, с таймаутом); не бросает исключений."""
        try:
            if self.dry_run:
                self.log.line('GUARD-DRYRUN tag=%s text=%s' % (tag, text))
                return
            if self.handler is None:
                self.log.line('GUARD-ERROR no handler, event lost tag=%s text=%s' % (tag, text))
                return
            res = await asyncio.wait_for(
                asyncio.get_running_loop().run_in_executor(
                    None, functools.partial(self.handler, tag, text, value, flags, self.cfg)),
                self.HANDLER_TIMEOUT)
            self.log.line('GUARD notify tag=%s text=%s -> %s' % (tag, text, res))
        except asyncio.CancelledError:
            raise
        except BaseException as e:
            self.log.line('GUARD-ERROR notify tag=%s text=%s: %r' % (tag, text, e))


def guard_self_test(args, log):
    """--guard-test TAG: отправить одно тестовое событие через обработчик, вернуть код выхода."""
    try:
        cfg = load_guard_config(args.guard_config)
        fn, desc = load_handler(cfg.get('handler'))
        print('GUARD loaded handler %s' % desc)
        res = fn(args.guard_test, 'тест eybond_tap guard', 0.0, 0, cfg)
        print('GUARD test tag=%s -> %s' % (args.guard_test, res))
        return 0
    except Exception as e:
        print('GUARD-ERROR test: %r' % e)
        return 1


def build_guard_runner(args, log):
    """Создаёт GuardRunner (guard и/или трекер) из args.settings либо None.
    Без args.settings (программный вызов) - старая схема через args.guard_config."""
    cfg = getattr(args, 'settings', None)
    if cfg is None:
        if not args.guard_config:
            return None
        try:
            cfg = load_guard_config(args.guard_config)
        except Exception as e:
            log.line('GUARD-ERROR cannot load config %s: %r (guard disabled)' % (args.guard_config, e))
            return None
        cfg['guard'] = cfg.get('enabled', True)
        cfg['guard_dry_run'] = bool(getattr(args, 'guard_dry_run', False))
    guard, tracker = bool(cfg.get('guard')), bool(cfg.get('tracker', False))
    regs = bool(cfg.get('regs_file'))
    if not guard and not tracker and not regs:
        log.line('GUARD disabled by config')
        return None
    if guard:
        log.line('GUARD enabled warn_v=%s alert_v=%s clear_v=%s consecutive=%s host=%s'
                 % (cfg['warn_v'], cfg['alert_v'], cfg['clear_v'], cfg['consecutive'], cfg['host']))
    else:
        log.line('GUARD disabled by config (tracker only)')
    if regs:
        log.line('REGS cache enabled regs_file=%s' % cfg.get('regs_file'))
    if tracker:
        log.line('TRACKER enabled cycle_gap_s=%s state_file=%s' % (cfg.get('cycle_gap_s'), cfg.get('state_file')))
    return GuardRunner(cfg, log, dry_run=bool(cfg.get('guard_dry_run')), notify=guard)


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
            guard = getattr(args, 'guard_runner', None)
            if guard is not None:
                guard.feed(cid, label, data)   # не бросает; пассивный разбор копии уже после отправки
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
    guard = getattr(args, 'guard_runner', None)
    if guard is not None:
        guard.conn_open(cid)
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
        if guard is not None:
            guard.conn_close(cid)
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

    guard = getattr(args, 'guard_runner', None)
    if guard is None and (getattr(args, 'settings', None) or getattr(args, 'guard_config', None)):
        guard = args.guard_runner = build_guard_runner(args, log)
    if guard is not None:
        guard.start()
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
    if guard is not None:
        await guard.stop()


def build_parser(full=True):
    """full=False: у всех опций нет умолчаний (argparse.SUPPRESS) - так определяем, что указано явно."""
    def d(v):
        return v if full else argparse.SUPPRESS
    p = argparse.ArgumentParser(description='Прозрачный TCP-tap для Eybond/SmartESS даталоггера. '
                                'Все параметры можно задать в --config (JSON); приоритет: флаг CLI > конфиг > умолчание.')
    p.add_argument('--config', default=None, metavar='PATH',
                   help='JSON-конфиг со ВСЕМИ параметрами (см. docstring и etc/eybond-tap.conf.example)')
    p.add_argument('--listen', default=d('0.0.0.0:502'), type=lambda s: parse_hostport(s, '--listen', 0),
                   help='куда слушать, HOST:PORT (по умолчанию 0.0.0.0:502; порт уточняется)')
    p.add_argument('--upstream', default=None, type=lambda s: parse_hostport(s, '--upstream'),
                   help='настоящий сервер HOST:PORT: IP (8.218.126.236:502) или имя (ess.eybond.com:502) - '
                        'имя резолвится напрямую через --dns, минуя локальный резолвер; '
                        'обязателен, если нет ключа upstream в конфиге')
    p.add_argument('--dns', default=d('8.8.8.8'), type=parse_dns,
                   help='DNS-сервер для имени upstream, HOST[:PORT] (по умолчанию 8.8.8.8)')
    p.add_argument('--log', default=d(None), help='файл лога (по умолчанию stdout)')
    p.add_argument('--max-hex', type=int, default=d(64), help='байт payload в hex на чанк (64)')
    p.add_argument('--burst-gap', type=float, default=d(3.0), help='пауза S->C, начинающая новый burst, с (3.0)')
    p.add_argument('--heuristic', action='store_true', default=d(False),
                   help='добавлять эвристический разбор первых 8 байт (непроверенный, только для наблюдения)')
    p.add_argument('--guard-config', default=None, metavar='PATH',
                   help='УСТАРЕЛО: синоним --config (guard включён)')
    p.add_argument('--guard-dry-run', action='store_true', default=d(False),
                   help="guard: только логировать 'GUARD-DRYRUN tag=.. text=..', обработчик не вызывать")
    p.add_argument('--guard-test', default=None, metavar='TAG',
                   help='guard: отправить одно тестовое событие через обработчик из --config и выйти')
    return p


def main(argv=None):
    parser = build_parser()
    if argv is None:
        argv = sys.argv[1:]
    args = resolve_settings(parser, argv)
    s = args.settings
    if args.guard_test is not None:
        if not args.config_path:
            parser.error('--guard-test требует --config')
        sys.exit(guard_self_test(args, None))
    if not s['upstream']:
        parser.error('не задан upstream: укажите --upstream HOST:PORT или ключ "upstream" в конфиге')
    args.listen, args.dns = s['listen'], s['dns']
    args.max_hex, args.burst_gap, args.heuristic = s['max_hex'], s['burst_gap'], s['heuristic']
    args.guard_dry_run = s['guard_dry_run']
    args.upstream = Upstream(s['upstream'][0], s['upstream'][1], s['dns'][0], s['dns'][1])
    try:
        log = Log(s['log'])
    except OSError as e:
        parser.error('не удалось открыть лог %s: %s' % (s['log'], e.strerror or e))
    try:
        for m in args.config_msgs:
            log.line(m)
        asyncio.run(run(args, log))
    finally:
        log.close()


if __name__ == '__main__':
    main()
