import copy, os, sys, tempfile, unittest
sys.path.insert(0, os.path.dirname(__file__))
import gs_nut_feeder as f

CONF = dict(f.DEFAULTS, rated_w=1800)
# shape of the real board /api/state (values anonymised)
OL = {'rt_ok': True, 'ongrid_reg': 1, 'relay_grid': 1, 'charge_a': 0.5, 'load_w': 0.0,
      'voltages': {'battery': 45.5, 'alt': 0.0, 'grid': 241.6},
      'decision': {'flags': 'SOFT_LOW,HOLD,SINGLE_SRC,CELL_LOW'},
      'battery': {'v': 45.7, 'soc': 40, 'cell_min_mv': 3100}}


def mod(**kw):
    s = copy.deepcopy(OL)
    for k, v in kw.items():
        if k == 'grid': s['voltages']['grid'] = v
        elif k == 'flags': s['decision']['flags'] = v
        elif k == 'soc': s['battery']['soc'] = v
        else: s[k] = v
    return s


class T(unittest.TestCase):
    def test_online_charging(self):
        v = f.compute(OL, CONF)
        self.assertEqual(v['ups.status'], 'OL CHRG')
        self.assertEqual(v['battery.charge'], '40'); self.assertEqual(v['input.voltage'], '241.6')
        self.assertEqual(v['ups.load'], '0')

    def test_detached_with_grid_present_is_on_battery(self):    # the failure of 2026-10-14 night
        v = f.compute(mod(ongrid_reg=0, load_w=580, charge_a=0.0), CONF)
        self.assertEqual(v['ups.status'], 'OB DISCHRG')
        self.assertEqual(v['ups.load'], '32')

    def test_grid_lost_and_low(self):
        v = f.compute(mod(ongrid_reg=0, grid=0.0, flags='HARD_LOW,CELL_HARD', soc=1), CONF)
        self.assertEqual(v['ups.status'], 'OB DISCHRG LB')

    def test_low_soc_alone(self):
        self.assertIn('LB', f.compute(mod(soc=10), CONF)['ups.status'].split())
        self.assertNotIn('LB', f.compute(mod(soc=11), CONF)['ups.status'].split())

    def test_grid_out_of_range(self):
        self.assertTrue(f.compute(mod(grid=150.0), CONF)['ups.status'].startswith('OB'))

    def test_untrustworthy(self):
        self.assertIsNone(f.compute(mod(rt_ok=False), CONF))
        self.assertIsNone(f.compute({}, CONF)); self.assertIsNone(f.compute(None, CONF))
        s = mod(); s['voltages'] = {'battery': 45.5}
        self.assertIsNone(f.compute(s, CONF))

    def test_render(self):
        txt = f.render(f.compute(OL, CONF))
        self.assertIn('ups.status: OL CHRG\n', txt); self.assertTrue(txt.endswith('\n'))

    def test_step_keeps_file_on_failure_and_alarms_when_stale(self):
        with tempfile.TemporaryDirectory() as d:
            conf = dict(CONF, out=os.path.join(d, 'u.dev'), stale_alarm_s=300)
            ok, txt = f.step(conf, lambda: OL, 0, 1000, '')
            body = open(conf['out']).read()
            self.assertIn('ups.status: OL CHRG', body)

            def boom(): raise OSError('down')
            ok2, txt2 = f.step(conf, boom, ok, 1100, txt)           # 100 s: untouched
            self.assertEqual(open(conf['out']).read(), body); self.assertEqual(ok2, 1000)
            f.step(conf, boom, ok2, 1400, txt2)                     # 400 s: alarm only, status unchanged
            b2 = open(conf['out']).read()
            self.assertIn('ups.alarm: board data stale 400 s', b2); self.assertIn('ups.status: OL CHRG', b2)
            f.step(conf, lambda: OL, ok2, 1500, txt2)               # back: alarm gone
            self.assertNotIn('ups.alarm', open(conf['out']).read())

    def test_fetch_state_local_server(self):
        import http.server, json, threading
        class H(http.server.BaseHTTPRequestHandler):
            def do_GET(s):
                b = json.dumps(OL).encode(); s.send_response(200); s.end_headers(); s.wfile.write(b)
            def log_message(*a): pass
        srv = http.server.HTTPServer(('127.0.0.1', 0), H)
        threading.Thread(target=srv.serve_forever, daemon=True).start()
        st = f.fetch_state('http://127.0.0.1:%d' % srv.server_port, timeout=3)
        srv.shutdown()
        self.assertEqual(st['voltages']['grid'], 241.6)


if __name__ == '__main__':
    unittest.main()
