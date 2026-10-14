import json, os, sys, tempfile, unittest
sys.path.insert(0, os.path.dirname(__file__))
import spp_health_monitor as m

ERR = '[%d.000000] usb 1-1.1.2: failed to send control message: -110\n'


class FakeIO(m.IO):
    def __init__(self):
        self.t, self.up = 1_000_000.0, 5000.0
        self.dmesg, self.files, self.units, self.posts, self.runs = '', {'/etc/telebot': 'http://srv.example'}, {}, [], []
        self.net_ok, self.board, self.exist, self.rc_recovery = True, None, set(), 0
    def now(self): return self.t
    def uptime(self): return self.up
    def read(self, p): return self.files[p] if p in self.files else (m.IO.read(self, p) if p.endswith('state.json') else None)
    def exists(self, p): return p in self.exist
    def run(self, cmd, timeout=30):
        self.runs.append(cmd)
        if cmd == 'dmesg': return 0, self.dmesg
        if cmd == 'vcgencmd get_throttled': return 1, ''
        if isinstance(cmd, list) and cmd[:2] == ['systemctl', 'is-active']: return (0, 'active') if self.units.get(cmd[2], True) else (3, 'failed')
        return self.rc_recovery, 'recovered'
    def http_get(self, url, timeout=8):
        if self.board is None: raise OSError('down')
        return json.dumps(self.board) if url.endswith('/api/state') else '1,BOOT rev=x reset=PANIC\n'
    def http_post(self, url, data, timeout=15):
        if not self.net_ok: raise OSError('net down')
        self.posts.append((url, data['event'])); return '<pre>ok'
    def disk_free_pct(self, path='/'): return 50.0
    def advance(self, s): self.t += s; self.up += s


def mk(io, **over):
    d = tempfile.mkdtemp()
    conf = m.merge(m.DEFAULTS, dict(state_dir=d, board={'url': 'http://b'}, services=['eybond-tap'], **over))
    return m.Monitor(conf, io), d


def journal(d):
    p = os.path.join(d, 'incidents.jsonl')
    if not os.path.exists(p): return []
    with open(p) as f: return [json.loads(l) for l in f]


class T(unittest.TestCase):
    def test_parse_and_patterns(self):
        lines = m.parse_dmesg('[ 12.5] hello\n[  99.000001] ch341-uart ttyUSB1: failed to read modem status: -110\nnoise\n')
        self.assertEqual(len(lines), 2)
        self.assertEqual(m.usb_error_times(lines), [99.000001])
        self.assertEqual(m.usb_error_times(lines, after=100), [])

    def test_usb_burst_triggers_recovery_alert_and_clears(self):
        io = FakeIO(); mon, d = mk(io)
        io.dmesg = ''.join(ERR % (4990 + i) for i in range(6))
        pr = mon.step()
        self.assertIn('usb:errors', pr)
        self.assertEqual([r for r in io.runs if r == '/usr/sbin/usb_recovery.sh'], ['/usr/sbin/usb_recovery.sh'])
        kinds = [(j['kind'], j.get('what')) for j in journal(d)]
        self.assertIn(('open', 'usb:errors'), kinds); self.assertIn(('action', 'usb_recovery'), kinds)
        self.assertTrue(any('USB' in e for _, e in io.posts))
        io.advance(200)                       # errors are older than the window and before recovery -> cleared
        mon.step()
        self.assertNotIn('usb:errors', mon.open)
        self.assertIn(('close', 'usb:errors'), [(j['kind'], j.get('what')) for j in journal(d)])
        self.assertTrue(any(e.startswith('OK') for _, e in io.posts))

    def test_few_errors_ignored(self):
        io = FakeIO(); mon, d = mk(io)
        io.dmesg = ''.join(ERR % (4990 + i) for i in range(3))
        self.assertNotIn('usb:errors', mon.step())

    def test_recovery_cooldown_and_giveup(self):
        io = FakeIO(); mon, d = mk(io, usb={'cooldown_s': 100, 'max_per_hour': 2})
        for _ in range(5):
            io.dmesg = ''.join(ERR % (io.up - 5 + i * 0.1) for i in range(6))
            mon.step(); io.advance(120)
        runs = [r for r in io.runs if r == '/usr/sbin/usb_recovery.sh']
        self.assertEqual(len(runs), 2)
        self.assertIn('usb:giveup', mon.open)

    def test_outbox_when_network_down_then_delivered(self):
        io = FakeIO(); mon, d = mk(io)
        io.net_ok = False
        io.dmesg = ''.join(ERR % (4990 + i) for i in range(6))
        mon.step()
        self.assertEqual(io.posts, []); self.assertTrue(mon.outbox)
        io.net_ok = True; io.advance(300); io.dmesg = ''
        mon.step()
        self.assertEqual(mon.outbox, [])
        self.assertTrue(any('мин назад' in e for _, e in io.posts))

    def test_service_down_and_back(self):
        io = FakeIO(); mon, d = mk(io)
        io.units['eybond-tap'] = False
        self.assertIn('svc:eybond-tap', mon.step())
        io.units['eybond-tap'] = True; io.advance(30)
        mon.step(); self.assertNotIn('svc:eybond-tap', mon.open)

    def test_board_checks(self):
        io = FakeIO(); mon, d = mk(io)
        ok = {'mode': 'normal', 'safe_mode': False, 'rt_ok': True, 'ongrid_reg': 1, 'uptime_ms': 100000,
              'voltages': {'grid': 240.0, 'battery': 51.0}}
        io.board = ok
        self.assertFalse([k for k in mon.step() if k.startswith('board')])
        io.board = dict(ok, mode='sniff'); io.advance(30)
        self.assertIn('board:mode', mon.step())
        io.board = dict(ok, uptime_ms=10000); io.advance(30)         # rebooted
        mon.step()
        self.assertTrue(any('перезагрузилась' in e and 'PANIC' in e for _, e in io.posts))

    def test_board_offgrid_with_grid_and_low_battery_after_delay(self):
        io = FakeIO(); mon, d = mk(io)
        st = {'mode': 'normal', 'safe_mode': False, 'rt_ok': True, 'ongrid_reg': 0, 'uptime_ms': 1,
              'voltages': {'grid': 242.0, 'battery': 47.0}}
        io.board = st
        self.assertNotIn('board:offgrid', mon.step())               # not yet 120 s
        io.advance(130)
        self.assertIn('board:offgrid', mon.step())
        io.board = dict(st, voltages={'grid': 242.0, 'battery': 53.0})   # solar day, battery fine
        io.advance(30)
        mon.step(); self.assertNotIn('board:offgrid', mon.open)

    def test_board_silent(self):
        io = FakeIO(); mon, d = mk(io)
        io.board = None
        self.assertNotIn('board:silent', mon.step())
        io.advance(301)
        self.assertIn('board:silent', mon.step())

    def test_expected_device_missing(self):
        io = FakeIO(); mon, d = mk(io, usb={'expect_devices': ['/dev/ttyUSB0']})
        self.assertIn('usb:dev:/dev/ttyUSB0', mon.step())
        io.exist.add('/dev/ttyUSB0'); io.advance(30)
        mon.step(); self.assertNotIn('usb:dev:/dev/ttyUSB0', mon.open)

    def test_renotify_and_state_persistence(self):
        io = FakeIO(); mon, d = mk(io)
        io.board = {'mode': 'normal', 'safe_mode': False, 'rt_ok': True, 'ongrid_reg': 1, 'uptime_ms': 10**9, 'voltages': {'grid': 240.0, 'battery': 51.0}}
        io.units['eybond-tap'] = False
        mon.step(); n1 = len(io.posts)
        io.advance(1800); mon.step()
        self.assertEqual(len(io.posts), n1)                          # not yet an hour
        io.advance(1900); mon.step()
        self.assertEqual(len(io.posts), n1 + 1)                      # renotify
        mon2 = m.Monitor(mon.c, io)                                  # restart keeps open incidents
        self.assertIn('svc:eybond-tap', mon2.open)

    def test_post_format(self):
        io = FakeIO(); mon, d = mk(io)
        mon.send('alert', 'a\nb')
        url, body = io.posts[0]
        self.assertTrue(url.startswith('http://srv.example/reg_event.php?tag=ALERT&host='))
        self.assertIn('value=0&flags=0', url); self.assertEqual(body, 'a\\nb')


if __name__ == '__main__':
    unittest.main()
