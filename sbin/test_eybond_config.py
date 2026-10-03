#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""Тесты полного конфиг-файла и трекера ритма опроса eybond_tap. Запуск: python3 test_eybond_config.py"""
import asyncio
import contextlib
import io
import json
import os
import random
import signal
import statistics
import subprocess
import sys
import tempfile
import time
import unittest

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import eybond_tap as tap  # noqa: E402
from test_eybond_tap import MemLog, mkargs, start_tap  # noqa: E402
from test_eybond_guard import req, resp, rt_regs  # noqa: E402

TAP_PY = os.path.join(os.path.dirname(os.path.abspath(__file__)), 'eybond_tap.py')


def load_json(p):
    with open(p, encoding='utf-8') as f:
        return json.load(f)


def read_text(p):
    with open(p, encoding='utf-8') as f:
        return f.read()


def write_cfg(d, obj, name='c.json'):
    p = os.path.join(d, name)
    with open(p, 'w', encoding='utf-8') as f:
        f.write(obj if isinstance(obj, str) else json.dumps(obj))
    return p


def resolve(argv):
    return tap.resolve_settings(tap.build_parser(), argv)


def resolve_fail(argv):
    err = io.StringIO()
    with contextlib.redirect_stderr(err):
        with unittest.TestCase().assertRaises(SystemExit) as cm:
            resolve(argv)
    return cm.exception.code, err.getvalue()


class ConfigTest(unittest.TestCase):
    def setUp(self):
        self._d = tempfile.TemporaryDirectory()
        self.d = self._d.name

    def tearDown(self):
        self._d.cleanup()

    def test_defaults_without_config(self):
        a = resolve(['--upstream', '1.2.3.4:502'])
        s = a.settings
        self.assertEqual((s['listen'], s['dns'], s['max_hex'], s['burst_gap']), (('0.0.0.0', 502), ('8.8.8.8', 53), 64, 3.0))
        self.assertFalse(s['guard'])
        self.assertTrue(s['tracker'])

    def test_precedence_file_cli_default(self):
        p = write_cfg(self.d, {'listen': '127.0.0.1:1502', 'upstream': 'ess.eybond.com:502', 'dns': '9.9.9.9:5353',
                               'max_hex': 16, 'burst_gap': 1.5, 'heuristic': True, 'log': '/x/y.log',
                               'warn_v': 52, 'cycle_gap_s': 7, 'state_file': '/tmp/s.json'})
        a = resolve(['--config', p])                       # upstream только из файла
        s = a.settings
        self.assertEqual(s['listen'], ('127.0.0.1', 1502))
        self.assertEqual(s['upstream'], ('ess.eybond.com', 502))
        self.assertEqual(s['dns'], ('9.9.9.9', 5353))
        self.assertEqual((s['max_hex'], s['burst_gap'], s['heuristic'], s['log']), (16, 1.5, True, '/x/y.log'))
        self.assertEqual((s['warn_v'], s['alert_v'], s['cycle_gap_s'], s['state_file']), (52, 49.0, 7, '/tmp/s.json'))
        self.assertTrue(s['guard'])                         # конфиг задан -> guard включён
        a = resolve(['--config', p, '--listen', '0.0.0.0:99', '--upstream', '5.6.7.8:1', '--max-hex', '8',
                     '--dns', '1.1.1.1', '--burst-gap', '0.5', '--log', '/z.log'])   # CLI > файл
        s = a.settings
        self.assertEqual((s['listen'], s['upstream'], s['max_hex'], s['dns'], s['burst_gap'], s['log']),
                         (('0.0.0.0', 99), ('5.6.7.8', 1), 8, ('1.1.1.1', 53), 0.5, '/z.log'))
        self.assertEqual(s['warn_v'], 52)                   # остальное - из файла

    def test_guard_switch_and_dry_run(self):
        p = write_cfg(self.d, {'upstream': '1.2.3.4:5', 'guard': False})
        self.assertFalse(resolve(['--config', p]).settings['guard'])
        p = write_cfg(self.d, {'upstream': '1.2.3.4:5', 'enabled': False}, 'e.json')   # прежнее имя ключа
        self.assertFalse(resolve(['--config', p]).settings['guard'])
        p = write_cfg(self.d, {'upstream': '1.2.3.4:5', 'guard_dry_run': False}, 'g.json')
        a = resolve(['--config', p, '--guard-dry-run'])
        self.assertTrue(a.settings['guard_dry_run'])
        self.assertTrue(a.settings['guard'])

    def test_guard_config_alias(self):
        p = write_cfg(self.d, {'upstream': '1.2.3.4:5', 'warn_v': 50.5})
        a = resolve(['--guard-config', p])
        self.assertTrue(a.settings['guard'])
        self.assertEqual(a.settings['warn_v'], 50.5)
        self.assertEqual(a.settings['upstream'], ('1.2.3.4', 5))
        self.assertTrue(any('deprecated' in m for m in a.config_msgs))
        self.assertEqual(a.guard_config, p)

    def test_unknown_key_and_loaded_line(self):
        p = write_cfg(self.d, {'upstream': '1.2.3.4:5', 'foo': 1, 'bar': 2})
        a = resolve(['--config', p])
        self.assertEqual(a.config_msgs[0], 'CONFIG loaded %s (3 keys)' % p)
        self.assertIn('CONFIG unknown key: foo', a.config_msgs)
        self.assertIn('CONFIG unknown key: bar', a.config_msgs)
        self.assertEqual(a.settings['foo'], 1)               # сохраняется для плагинов

    def test_bad_configs(self):
        cases = [
            ({'listen': '0.0.0.0:99999'}, 'listen'),
            ({'upstream': 'host'}, 'upstream'),
            ({'dns': 5}, 'dns'),
            ({'warn_v': -1}, 'warn_v'),
            ({'warn_v': 48, 'alert_v': 49}, 'alert_v'),
            ({'warn_v': 49, 'alert_v': 49}, 'warn_v'),
            ({'clear_v': 50}, 'clear_v'),
            ({'consecutive': 0}, 'consecutive'),
            ({'consecutive': 1.5}, 'consecutive'),
            ({'cycle_gap_s': 0}, 'cycle_gap_s'),
            ({'burst_gap': 'x'}, 'burst_gap'),
            ({'max_hex': -3}, 'max_hex'),
            ({'tracker': 'yes'}, 'tracker'),
            ({'guard': 1}, 'guard'),
            ({'stale_s': True}, 'stale_s'),
            ({'handler': 7}, 'handler'),
            ({'alert_webhook_url': 'ftp://x/y'}, 'alert_webhook_url'),
            ({'alert_webhook_url': 'http://h/?a=${bogus}'}, 'bogus'),
            ({'alert_webhook_method': 'PUT'}, 'alert_webhook_method'),
            ({'alert_webhook_body': 'x=${nope}'}, 'nope'),
            ({'alert_webhook_timeout_s': 0}, 'alert_webhook_timeout_s'),
            ([1, 2], 'JSON'),
            ('{broken', 'JSON'),
        ]
        for i, (obj, word) in enumerate(cases):
            base = dict(obj, upstream=obj.get('upstream', '1.2.3.4:5')) if isinstance(obj, dict) else obj
            p = write_cfg(self.d, base, 'b%d.json' % i)
            code, err = resolve_fail(['--config', p])
            self.assertEqual(code, 2, obj)
            self.assertIn(word, err, (obj, err))
            self.assertNotIn('Traceback', err)
        code, err = resolve_fail(['--config', os.path.join(self.d, 'nope.json')])
        self.assertEqual(code, 2)
        self.assertIn('nope.json', err)

    def test_webhook_keys_and_typo_alias(self):
        url = 'http://h/e?m=${event_message}&v=${event_value}&h=${host_name}&t=${event_tag}&f=${event_flags}&ts=${event_time}'
        p = write_cfg(self.d, {'upstream': '1.2.3.4:5', 'alert_webook_url': url, 'alert_webhook_method': 'post'})
        args = resolve(['--config', p])
        self.assertEqual(args.settings['alert_webhook_url'], url)          # опечатка webook принимается
        self.assertEqual(args.settings['alert_webhook_method'], 'POST')
        self.assertFalse([m for m in args.config_msgs if 'unknown' in m])

    def test_main_missing_upstream(self):
        r = subprocess.run([sys.executable, TAP_PY, '--listen', '127.0.0.1:0'], capture_output=True, text=True, timeout=20)
        self.assertNotEqual(r.returncode, 0)
        self.assertIn('upstream', r.stderr)
        self.assertNotIn('Traceback', r.stderr)
        p = write_cfg(self.d, {'upstream': '1.2.3.4:5', 'warn_v': 40})
        r = subprocess.run([sys.executable, TAP_PY, '--config', p], capture_output=True, text=True, timeout=20)
        self.assertNotEqual(r.returncode, 0)
        self.assertNotIn('Traceback', r.stderr)

    @unittest.skipIf(sys.platform == 'win32', 'корректное завершение по SIGINT проверяется только на POSIX')
    def test_main_runs_from_config_only(self):
        logp = os.path.join(self.d, 'tap.log')
        p = write_cfg(self.d, {'listen': '127.0.0.1:0', 'upstream': '127.0.0.1:9', 'log': logp, 'foo': 1,
                               'guard': False})
        proc = subprocess.Popen([sys.executable, TAP_PY, '--config', p])
        try:
            for _ in range(100):
                if os.path.exists(logp) and 'LISTEN' in read_text(logp):
                    break
                time.sleep(0.1)
        finally:
            proc.send_signal(signal.SIGTERM)
            proc.wait(10)
        out = read_text(logp)
        self.assertIn('CONFIG loaded %s (5 keys)' % p, out)
        self.assertIn('CONFIG unknown key: foo', out)
        self.assertIn('TRACKER enabled', out)
        self.assertIn('SHUTDOWN', out)


# ---------------- трекер ----------------
BLOCKS = [(20001, 16), (20213, 2), (25201, 74), (20001, 16), (20213, 2), (20101, 5), (20301, 8), (20401, 4),
          (25201, 74), (20501, 6)]


def synth_cycle(feed, t0, pv_dv=523, rng=None):
    """10 запросов с шагом ~0.3 с (+ ответ через 0.05 с) начиная с t0; feed(t, direction, bytes)."""
    t = t0
    for addr, cnt in BLOCKS:
        feed(t, 'S->C', b'\x00\x01\x00\x00\x00\x06\x00\x00' + req(addr, cnt))   # произвольный 8-байтный заголовок
        regs = rt_regs(pv_dv, n=cnt) if addr == 25201 else [0] * cnt
        feed(t + 0.05, 'C->S', resp(regs))
        t += 0.3 + (rng.uniform(-0.02, 0.02) if rng else 0)
    return t


class TrackerTest(unittest.TestCase):
    def timeline(self, n=12, seed=1):
        rng = random.Random(seed)
        starts, t = [], 1000.0
        for _ in range(n):
            starts.append(t)
            t += rng.uniform(82, 97)
        return starts

    def test_synthetic_timeline_locked(self):
        T = [0.0]
        wall0 = 1_800_000_000.0
        tr = tap.CycleTracker(10.0, clock=lambda: T[0], wall=lambda: wall0 + T[0])
        g = tap.Guard()

        class Obs:
            def on_request(s, now, a, c):
                tr.request(now, a, c)

            def on_response(s, now, a, r):
                tr.response(now, a, r)
        g.observer = Obs()
        closed = []

        def feed(t, d, data):
            T[0] = t
            g.feed(t, 1, d, data)

        hb_state = {'next': 1000.0}
        starts = self.timeline(13)
        for st in starts[:12]:
            # heartbeat 16/26 байт каждые ~36 с до этого цикла - игнорируется
            while hb_state['next'] < st:
                feed(hb_state['next'], 'S->C', bytes([4, 3, 0x62, 0x71]) + bytes(12))      # 16 Б, выглядит как начало запроса
                feed(hb_state['next'] + 0.1, 'C->S', bytes(26))
                hb_state['next'] += 36
            end = synth_cycle(feed, st)
            T[0] = end + 10.5
            rec = tr.tick(T[0])
            self.assertIsNotNone(rec)
            closed.append(rec)
        self.assertEqual(tr.cycles_seen, 12)
        for rec in closed:
            self.assertEqual(rec['n_req'], 10)
            self.assertEqual(rec['fresh'], 10)
            self.assertAlmostEqual(rec['pack_v'], 52.3)
            self.assertAlmostEqual(rec['dur'], 3.0, delta=0.5)
        period, jitter, locked = tr.stats()
        iv = [b - a for a, b in zip(starts, starts[1:12])][-8:]
        self.assertTrue(locked)
        self.assertAlmostEqual(period, statistics.median(iv), places=2)
        self.assertTrue(82 <= period <= 97)
        self.assertLessEqual(jitter, 15.0)
        # предсказание следующего старта
        st = tr.state(T[0])
        pred = st['next_start_epoch']
        actual = wall0 + starts[12]
        self.assertLessEqual(abs(actual - pred), jitter + 1.0, (actual - pred, jitter))
        self.assertTrue(st['locked'])
        self.assertEqual(st['cycles_seen'], 12)
        self.assertAlmostEqual(st['last_pack_v'], 52.3)
        self.assertAlmostEqual(st['last_start_epoch'], wall0 + starts[11], places=3)
        self.assertTrue(st['last_start_utc'].endswith('Z'))
        self.assertAlmostEqual(st['last_sample_age_s'], T[0] - (starts[11] + 0.3 * 8 + 0.05), delta=1.0)
        line = tap.CycleTracker.format(closed[-1])
        self.assertIn('n_req=10', line)
        self.assertIn('fresh=10/10', line)
        self.assertIn('blocks=20001x16,20213x2,25201x74', line)
        self.assertIn('pack_v=52.3', line)
        self.assertIn('locked=true', line)

    def test_not_locked_with_few_cycles_and_unfresh(self):
        tr = tap.CycleTracker(10.0)
        tr.request(0.0, 25201, 74)
        tr.request(0.3, 20001, 16)
        tr.response(0.35, 20001, [0] * 16)
        rec = tr.tick(20.0)
        self.assertEqual((rec['n_req'], rec['fresh']), (2, 1))
        self.assertIsNone(rec['pack_v'])
        self.assertIsNone(rec['period'])
        self.assertFalse(rec['locked'])
        self.assertIn('pack_v=? since_prev=? period=? locked=false', tap.CycleTracker.format(rec))
        self.assertIsNone(tr.tick(40.0))
        tr.request(100.0, 20001, 16)
        self.assertEqual(tr.tick(200.0)['since_prev'], 100.0)

    def test_runner_log_state_file_and_errors(self):
        with tempfile.TemporaryDirectory() as d:
            sf = os.path.join(d, 'state.json')
            T = [0.0]
            log = MemLog()
            cfg = dict(tap.CONFIG_DEFAULTS, host='h', cycle_gap_s=10, state_file=sf)
            r = tap.GuardRunner(cfg, log, dry_run=True, clock=lambda: T[0], wall=lambda: 1_800_000_000 + T[0], notify=False)
            starts = self.timeline(5, seed=3)

            def feed(t, d, data):
                T[0] = t
                r.feed(1, d, data)
            for st in starts:
                synth_cycle(feed, st)
            T[0] = starts[-1] + 20
            r._cycle_closed(r.tracker.tick(T[0]))
            out = log.fh.getvalue()
            self.assertEqual(out.count(' CYCLE start='), 5, out)
            self.assertIn('fresh=10/10 pack_v=52.3', out)
            st = load_json(sf)
            self.assertEqual(st['cycles_seen'], 5)
            for k in ('updated_utc', 'last_start_utc', 'last_start_epoch', 'period_s', 'jitter_s', 'locked',
                      'next_start_epoch', 'last_pack_v', 'last_sample_age_s', 'cycles_seen'):
                self.assertIn(k, st)
            self.assertFalse([f for f in os.listdir(d) if f != 'state.json'])   # tmp не остаётся
            # ошибка записи не ломает ничего
            r.state_file = os.path.join(d, 'no', 'such', 'dir', 's.json')
            r.tracker.request(T[0] + 100, 20001, 16)
            r._cycle_closed(r.tracker.tick(T[0] + 200))
            self.assertIn('TRACKER-ERROR state_file', log.fh.getvalue())

    def test_tracker_exception_contained(self):
        log = MemLog()
        r = tap.GuardRunner(dict(tap.CONFIG_DEFAULTS, host='h'), log, dry_run=True, notify=False)

        def boom(*a):
            raise RuntimeError('x')
        r.tracker.request = boom
        r.feed(1, 'S->C', req(25201, 74))     # не бросает
        self.assertIn('TRACKER-ERROR request', log.fh.getvalue())


class TrackerE2E(unittest.IsolatedAsyncioTestCase):
    async def test_cycle_through_tap(self):
        seq = [(20001, 16), (20213, 2), (25201, 74), (20101, 5)]
        hdr = b'\x12\x34\x00\x01\x00\x0e\x04\x03'
        stick_rx, cloud_rx = bytearray(), bytearray()

        def answer(addr, cnt):
            return resp(rt_regs(523, n=cnt) if addr == 25201 else [0] * cnt)

        async def cloud(r, w):
            for addr, cnt in seq:
                w.write(hdr + req(addr, cnt))
                await w.drain()
                cloud_rx.extend(await r.readexactly(len(answer(addr, cnt))))
                await asyncio.sleep(0.05)
            await asyncio.sleep(2)
            w.close()

        srv = await asyncio.start_server(cloud, '127.0.0.1', 0)
        up = srv.sockets[0].getsockname()[1]
        log = MemLog()
        args = mkargs(up)
        sf = tempfile.mktemp(suffix='.json')
        args.settings = dict(tap.CONFIG_DEFAULTS, guard=False, enabled=False, tracker=True, cycle_gap_s=0.4,
                             host='h', state_file=sf)
        stop, task, port = await start_tap(args, log)
        r, w = await asyncio.open_connection('127.0.0.1', port)
        for addr, cnt in seq:
            stick_rx.extend(await asyncio.wait_for(r.readexactly(len(hdr) + 8), 5))
            w.write(answer(addr, cnt))
            await w.drain()
        for _ in range(100):
            if ' CYCLE start=' in log.fh.getvalue():
                break
            await asyncio.sleep(0.05)
        out = log.fh.getvalue()
        w.close()
        await asyncio.sleep(0.1)
        stop.set()
        await task
        srv.close()
        line = next(l for l in out.splitlines() if ' CYCLE start=' in l)
        self.assertIn('n_req=4', line)
        self.assertIn('blocks=20001x16,20213x2,25201x74,20101x5', line)
        self.assertIn('fresh=4/4', line)
        self.assertIn('pack_v=52.3', line)
        self.assertIn('TRACKER enabled', out)
        self.assertEqual(load_json(sf)['cycles_seen'], 1)
        os.unlink(sf)
        self.assertEqual(len(cloud_rx), sum(len(answer(a, c)) for a, c in seq))   # проксирование не затронуто
        self.assertEqual(len(stick_rx), 4 * (len(hdr) + 8))


if __name__ == '__main__':
    unittest.main()
