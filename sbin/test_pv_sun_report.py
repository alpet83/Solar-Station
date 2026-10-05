import os, sys, tempfile, unittest
from datetime import date, datetime, timezone
sys.path.insert(0, os.path.dirname(__file__))
import pv_sun_report as p

HDR = 'epoch,pack_v,alt_v,grid_v,load_w,chg_a,chg_w,pv_v,pv_relay,relay_grid,ongrid_reg,bms_v,bms_soc,cell_min_mv\n'


def mk(rel, t0=1000000):
    """rel: list of 0/1 per 10 s sample"""
    return HDR + ''.join('%d,51.0,,,,,%d,100,%d,,,,,\n' % (t0 + 10 * i, 300 if r else 0, r) for i, r in enumerate(rel))


class T(unittest.TestCase):
    def test_sun_limassol(self):
        r, s = p.sun_times(date(2026, 10, 5), 34.68, 33.04)
        f = lambda e: datetime.fromtimestamp(e, timezone.utc).hour * 60 + datetime.fromtimestamp(e, timezone.utc).minute
        self.assertLess(abs(f(r) - (3 * 60 + 40)), 8)
        self.assertLess(abs(f(s) - (15 * 60 + 22)), 8)

    def test_polar(self):
        self.assertIsNone(p.sun_times(date(2026, 6, 21), 80.0, 10.0))

    def test_iso6709(self):
        self.assertAlmostEqual(p.parse_iso6709('+3510+03322')[0], 35.1667, 3)
        self.assertAlmostEqual(p.parse_iso6709('+513030-0000731')[1], -0.1253, 3)

    def test_runs(self):
        rows = p.parse_csv(mk([0, 1, 1, 0, 0] + [1] * 70 + [0, 0]))
        runs = p.find_runs(rows, 600)
        self.assertEqual(len(runs), 2)
        self.assertFalse(runs[0]['stable']); self.assertEqual(runs[0]['dur'], 20)
        self.assertTrue(runs[1]['stable'])

    def test_open_run_and_fallback(self):
        csv_ = HDR + ''.join('%d,51,,,,,%d,100,,,,,,\n' % (1000 + 10 * i, 50) for i in range(5))   # relay unknown -> chg_w
        runs = p.find_runs(p.parse_csv(csv_), 600)
        self.assertEqual(len(runs), 1); self.assertTrue(runs[0]['open'])

    def test_summary_offsets(self):
        t0 = 1000000
        rows = p.parse_csv(mk([1, 1, 0] + [1] * 70 + [0], t0))
        s = p.summarize(date(1970, 1, 12), rows, 600, (t0 - 600, t0 + 3600))
        self.assertEqual(s['runs'], 2); self.assertEqual(s['short_runs'], 1)
        self.assertEqual(s['start_off_min'], 10.5)   # stable run starts at t0+30 s, sunrise = t0-600 s
        
    def test_history_idempotent_and_recommend(self):
        with tempfile.TemporaryDirectory() as d:
            path = os.path.join(d, 'days.csv')
            for i in range(6):
                row = {k: '' for k in p.HIST_FIELDS}
                row.update(date='2026-10-%02d' % (i + 1), start_off_min=30 + i, end_off_min=20 + i)
                hist = p.update_history(path, row)
            hist = p.update_history(path, row)
            self.assertEqual(len(hist), 6)
            rec = p.recommend(hist)
            self.assertEqual(rec[2], 6); self.assertGreaterEqual(rec[0], 33)
            self.assertIsNone(p.recommend(hist[:3]))

    def test_location_config_and_tz(self):
        self.assertEqual(p.location({'lat': 1, 'lon': 2}), (1.0, 2.0, 'config'))
        c = p.zone_coords('Asia/Nicosia')
        if c:
            self.assertAlmostEqual(c[0], 35.17, 1)


class G(unittest.TestCase):
    def test_gpsd(self):
        import socket, threading
        srv = socket.socket(); srv.bind(('127.0.0.1', 0)); srv.listen(1); port = srv.getsockname()[1]

        def serve():
            c, _ = srv.accept(); c.recv(100)
            c.sendall(b'{"class":"VERSION"}\n{"class":"TPV","mode":1}\n{"class":"TPV","mode":3,"lat":12.345,"lon":67.891}\n')
            c.close()
        threading.Thread(target=serve, daemon=True).start()
        self.assertEqual(p.gpsd_fix(port=port, timeout=3), (12.345, 67.891))
        self.assertIsNone(p.gpsd_fix(port=1, timeout=1))


if __name__ == '__main__':
    unittest.main()
