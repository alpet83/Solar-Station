#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""Тесты battery guard в eybond_tap. Запуск: python3 test_eybond_guard.py"""
import asyncio
import os
import struct
import sys
import tempfile
import unittest

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import eybond_tap as tap  # noqa: E402
from test_eybond_tap import MemLog, mkargs, start_tap  # noqa: E402


def req(addr, cnt, fn=3, sid=4):
    return tap.mk_frame(bytes([sid, fn, addr >> 8, addr & 255, cnt >> 8, cnt & 255]))


def resp(regs, fn=3, sid=4):
    return tap.mk_frame(bytes([sid, fn, 2 * len(regs)]) + struct.pack('>%dH' % len(regs), *regs))


def rt_regs(pack_dv=523, grid_dv=2301, load=350, relay=1, n=74):
    r = [0] * n
    r[4], r[6], r[14], r[37] = pack_dv, grid_dv, load, relay
    return r


class ScanTest(unittest.TestCase):
    def test_crc_known(self):
        self.assertEqual(req(25201, 74), bytes.fromhex('04 03 62 71 00 4a'.replace(' ', '')) + req(25201, 74)[6:])
        self.assertEqual(tap.modbus_crc(bytes.fromhex('010300000001')), 0x0A84)  # 84 0A

    def test_split_glued_garbage(self):
        g = tap.Guard({'consecutive': 1})
        rq, rs = req(25201, 74), resp(rt_regs(pack_dv=485))
        ev = []
        stream_sc = b'\x00\x11\x22' + rq          # мусор-префикс
        ev += g.feed(1, 1, 'S->C', stream_sc[:5])  # режем запрос
        ev += g.feed(1, 1, 'S->C', stream_sc[5:])
        blob = b'\xde\xad\x04' + rs + b'\x01'
        for i in range(0, len(blob), 17):          # режем ответ на куски
            ev += g.feed(2, 1, 'C->S', blob[i:i + 17])
        self.assertEqual(g.sample_seq, 1)
        self.assertAlmostEqual(g.last_sample['pack_v'], 48.5)
        self.assertEqual([e[0] for e in ev], ['ALERT'])
        # склеенные два запроса + два ответа
        g2 = tap.Guard()
        g2.feed(0, 1, 'S->C', rq + req(25201, 74))
        g2.feed(1, 1, 'C->S', rs + resp(rt_regs()))
        self.assertEqual(g2.sample_seq, 2)

    def test_bytecount_rule_and_expiry(self):
        g = tap.Guard()
        g.feed(0, 1, 'S->C', req(25201, 74))
        g.feed(1, 1, 'C->S', resp([0] * 10))      # byteCount не совпал
        self.assertEqual(g.sample_seq, 0)
        bad = bytearray(resp(rt_regs()))
        bad[10] ^= 1                                    # битый CRC
        g.feed(1, 1, 'C->S', bytes(bad))
        self.assertEqual(g.sample_seq, 0)
        g.feed(100, 1, 'C->S', resp(rt_regs()))        # запрос протух (>30 c)
        self.assertEqual(g.sample_seq, 0)
        g.feed(101, 1, 'S->C', req(25201, 74))
        g.feed(102, 1, 'C->S', resp(rt_regs(), fn=4))  # fn не совпал
        self.assertEqual(g.sample_seq, 0)
        g.feed(103, 1, 'C->S', resp(rt_regs()))
        self.assertEqual(g.sample_seq, 1)

    def test_other_slave_ignored(self):
        g = tap.Guard()
        g.feed(0, 1, 'S->C', req(25201, 74, sid=1))
        g.feed(0, 1, 'C->S', resp(rt_regs(), sid=1))
        self.assertEqual(g.sample_seq, 0)


class DecodeTest(unittest.TestCase):
    def test_rt_block(self):
        g = tap.Guard()
        g.feed(0, 1, 'S->C', req(25201, 74))
        g.feed(0, 1, 'C->S', resp(rt_regs(523, 2301, 350, 1)))
        s = g.last_sample
        self.assertAlmostEqual(s['pack_v'], 52.3)
        self.assertAlmostEqual(s['grid_v'], 230.1)
        self.assertEqual((s['load_w'], s['relay_grid']), (350, 1))
        self.assertEqual(g.grid_text(), 'сеть подключена')

    def test_cfg_block_and_short_ignored(self):
        g = tap.Guard()
        g.feed(0, 1, 'S->C', req(20101, 5) + req(25201, 7))
        g.feed(0, 1, 'C->S', resp([0, 0, 0, 0, 0]) + resp([0] * 7))
        self.assertEqual(g.grid, {'ongrid_reg': 0})
        self.assertEqual(g.grid_text(), 'работа от батареи')
        self.assertEqual(g.sample_seq, 0)


def drive(g, t, pv, relay=1, cid=1):
    g.feed(t, cid, 'S->C', req(25201, 74))
    return g.feed(t, cid, 'C->S', resp(rt_regs(int(round(pv * 10)), relay=relay)))


class StateTest(unittest.TestCase):
    def test_flow(self):
        g = tap.Guard()
        g.conn_open(0, 1)
        for i, v in enumerate((55.0, 53.0, 52.0, 51.6)):
            self.assertEqual(drive(g, i, v), [])
        self.assertEqual(drive(g, 10, 51.4), [])                       # 1-й замер
        ev = drive(g, 11, 51.3)
        self.assertEqual([e[0] for e in ev], ['WARN'])
        self.assertIn('51.3', ev[0][1])
        self.assertIn('сеть подключена', ev[0][1])
        self.assertIn('нагрузка 350', ev[0][1])
        self.assertEqual(drive(g, 100, 51.0), [])                      # cooldown 3600
        self.assertEqual(drive(g, 101, 51.0), [])
        self.assertEqual(drive(g, 11 + 3600, 51.0)[0][0], 'WARN')      # cooldown прошёл
        self.assertEqual(drive(g, 5000, 48.9), [])                     # 1-й alert-замер
        ev = drive(g, 5001, 48.8, relay=0)
        self.assertEqual([e[0] for e in ev], ['ALERT'])                # эскалация сразу
        self.assertIn('работа от батареи', ev[0][1])
        self.assertEqual(drive(g, 5100, 48.7), [])                     # cooldown alert 900
        self.assertEqual(drive(g, 5001 + 900, 48.7)[0][0], 'ALERT')
        self.assertEqual(drive(g, 6000, 50.0), [])                     # деэскалация молча
        self.assertEqual(drive(g, 6001, 50.0), [])
        self.assertEqual(drive(g, 6002, 52.0), [])                     # между warn и clear
        ev = drive(g, 6003, 52.6)
        self.assertEqual([e[0] for e in ev], ['INFO'])
        self.assertEqual(drive(g, 6004, 53.0), [])                     # повтор INFO нет

    def test_no_recovery_without_notify(self):
        g = tap.Guard()
        drive(g, 0, 51.4)
        self.assertEqual(drive(g, 1, 53.0), [])

    def test_out_of_range_ignored(self):
        g = tap.Guard({'consecutive': 1})
        self.assertEqual(drive(g, 0, 12.0), [])
        self.assertEqual(drive(g, 1, 80.0), [])
        self.assertEqual(g.sample_seq, 0)

    def test_stale_and_offline(self):
        g = tap.Guard()
        self.assertEqual(g.tick(10000), [])                  # соединения не было - молчим
        g.conn_open(0, 1)
        self.assertEqual(g.tick(1000), [])
        ev = g.tick(1200)
        self.assertEqual(ev[0][0], 'WARN')
        self.assertIn('no battery data', ev[0][1])
        self.assertEqual(g.tick(1300), [])                   # cooldown
        drive(g, 1400, 55.0)
        self.assertEqual(g.tick(1500), [])                   # данные пошли
        g.conn_close(2000, 1)
        self.assertEqual(g.tick(3000), [])
        ev = g.tick(2000 + 1800)
        self.assertIn('stick offline', ev[0][1])
        self.assertEqual(g.tick(2000 + 1900), [])
        self.assertEqual(g.conns, {})                        # буферы выброшены


class HandlerTest(unittest.IsolatedAsyncioTestCase):
    def test_load_from_file_and_builtin(self):
        with tempfile.TemporaryDirectory() as d:
            p = os.path.join(d, 'h.py')
            with open(p, 'w') as f:
                f.write("def h(tag, text, value, flags, cfg):\n    return 'ok:%s:%s' % (tag, cfg['handler_args'])\n")
            fn, desc = tap.load_handler(p + ':h')
            self.assertEqual(fn('X', 't', 1.0, 0, {'handler_args': 'a'}), 'ok:X:a')
        fn, _ = tap.load_handler('builtin:webhook')
        self.assertIs(fn, tap.webhook_handler)
        fn, _ = tap.load_handler(None)
        self.assertIs(fn, tap.webhook_handler)
        fn, _ = tap.load_handler('os.path:basename')
        self.assertEqual(fn('/a/b'), 'b')
        with self.assertRaises(Exception):
            tap.load_handler('nonexistent_mod_xyz:f')

    async def test_failing_handler_contained(self):
        log = MemLog()

        def boom(*a):
            raise RuntimeError('nope')
        r = tap.GuardRunner(dict(tap.GUARD_DEFAULTS, host='h'), log, handler=boom)
        await r.deliver('ALERT', 'x', 1.0, 0)
        self.assertIn('GUARD-ERROR notify tag=ALERT', log.fh.getvalue())
        got = []
        r.handler = lambda *a: got.append(a) or 'fine'
        await r.deliver('WARN', 'y', 2.0, 0)
        self.assertEqual(got[0][0], 'WARN')
        self.assertIn('GUARD notify tag=WARN text=y -> fine', log.fh.getvalue())

    async def test_dryrun(self):
        log = MemLog()
        r = tap.GuardRunner(dict(tap.GUARD_DEFAULTS, host='h'), log, dry_run=True)
        await r.deliver('INFO', 'z', 0.0, 0)
        self.assertIn('GUARD-DRYRUN tag=INFO text=z', log.fh.getvalue())

    def _server(self, seen):
        import http.server
        import threading

        class H(http.server.BaseHTTPRequestHandler):
            def _do(self):
                seen['method'] = self.command
                seen['path'] = self.path
                n = int(self.headers.get('Content-Length') or 0)
                seen['body'] = self.rfile.read(n) if n else b''
                seen['ctype'] = self.headers.get('Content-Type')
                self.send_response(seen.get('status', 200))
                self.end_headers()
                self.wfile.write(seen.get('resp', b'OK'))
            do_GET = do_POST = _do

            def log_message(self, *a):
                pass
        srv = http.server.HTTPServer(('127.0.0.1', 0), H)
        threading.Thread(target=srv.serve_forever, daemon=True).start()
        return srv

    def test_webhook_get_placeholders(self):
        seen = {}
        srv = self._server(seen)
        url = 'http://127.0.0.1:%d/bot/reg_event.php?tag=${event_tag}&host=${host_name}&value=${event_value}&event=${event_message}&t=SECRET' % srv.server_port
        cfg = dict(tap.GUARD_DEFAULTS, host='h h', alert_webhook_url=url)
        res = tap.webhook_handler('WARN', 'привет & мир', 50.5, 0, cfg)
        self.assertEqual(seen['method'], 'GET')
        self.assertTrue(seen['path'].startswith('/bot/reg_event.php?tag=WARN&host=h%20h&value=50.5&event=%D0%BF'))
        self.assertIn('%26', seen['path'])                       # '&' в тексте закодирован
        self.assertIn('&t=SECRET', seen['path'])
        self.assertNotIn('SECRET', res)                          # токен в результат/лог не попадает
        self.assertNotIn('?', res.split('->')[0])
        srv.shutdown()

    def test_webhook_post_body_error_text_and_status(self):
        seen = {}
        srv = self._server(seen)
        cfg = dict(tap.GUARD_DEFAULTS, host='hh', alert_webhook_method='POST',
                   alert_webhook_url='http://127.0.0.1:%d/x' % srv.server_port,
                   alert_webhook_body='event=${event_message}&v=${event_value}',
                   alert_webhook_error_text='#ERROR')
        self.assertIn('POST', tap.webhook_handler('ALERT', 'a b', 48.5, 0, cfg))
        self.assertEqual(seen['body'], b'event=a%20b&v=48.5')
        self.assertEqual(seen['ctype'], 'application/x-www-form-urlencoded')
        seen['resp'] = b'#ERROR bad'
        with self.assertRaises(RuntimeError):
            tap.webhook_handler('WARN', 'x', 1, 0, cfg)
        seen['resp'] = b'OK'; seen['status'] = 500
        with self.assertRaises(RuntimeError):
            tap.webhook_handler('WARN', 'x', 1, 0, cfg)
        srv.shutdown()

    def test_webhook_not_configured(self):
        with self.assertRaises(ValueError):
            tap.webhook_handler('WARN', 'x', 1, 0, dict(tap.GUARD_DEFAULTS))

    def test_render_template(self):
        self.assertEqual(tap.render_template('a=${event_tag}&b=${host_name}', {'event_tag': 'X Y', 'host_name': 'h'}), 'a=X%20Y&b=h')
        with self.assertRaises(ValueError):
            tap.render_template('${nope}', {})


class E2ETest(unittest.IsolatedAsyncioTestCase):
    async def test_alert_through_tap(self):
        rq, rs = req(25201, 74), resp(rt_regs(485))
        cloud_rx, stick_rx = bytearray(), bytearray()

        async def cloud(r, w):                      # "облако": шлёт запросы (с мусорным префиксом), читает ответы
            for _ in range(2):
                w.write(b'\xaa\xbb' + rq)
                await w.drain()
                cloud_rx.extend(await r.readexactly(len(rs)))
                await asyncio.sleep(0.05)
            await asyncio.sleep(0.3)
            w.close()

        srv = await asyncio.start_server(cloud, '127.0.0.1', 0)
        up = srv.sockets[0].getsockname()[1]
        log = MemLog()
        args = mkargs(up)
        fired = []
        runner = tap.GuardRunner(dict(tap.GUARD_DEFAULTS, host='h'), log,
                                 handler=lambda *a: fired.append(a) or 'sent')
        args.guard_runner = runner
        stop, task, port = await start_tap(args, log)
        r, w = await asyncio.open_connection('127.0.0.1', port)
        for _ in range(2):                          # "стик": получил запрос -> отвечает
            stick_rx.extend(await asyncio.wait_for(r.readexactly(len(rq) + 2), 5))
            w.write(rs)
            await w.drain()
        for _ in range(50):
            if fired:
                break
            await asyncio.sleep(0.05)
        w.close()
        await asyncio.sleep(0.2)
        stop.set()
        await task
        srv.close()
        self.assertEqual(bytes(cloud_rx), rs * 2)               # байты не изменены
        self.assertEqual(bytes(stick_rx), (b'\xaa\xbb' + rq) * 2)
        self.assertEqual(len(fired), 1)
        self.assertEqual(fired[0][0], 'ALERT')
        self.assertAlmostEqual(fired[0][2], 48.5)
        out = log.fh.getvalue()
        self.assertIn('GUARD notify tag=ALERT', out)
        self.assertIn('GUARD sample pack_v=48.5', out)


if __name__ == '__main__':
    unittest.main()
