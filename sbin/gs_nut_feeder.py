#!/usr/bin/env python3
"""NUT feeder: publishes the grid-supervisor board state as a UPS for NUT's stock `dummy-ups` driver.

Every `interval_s` it reads GET {board_url}/api/state and rewrites the dummy-ups definition file `out`
(atomically; dummy-ups re-reads it when it changes). upsd/upsmon on any host then see a UPS named in
ups.conf (see etc/nut/ups.conf.example). Dependencies: Python 3.9+ standard library only.

Status mapping (what upsmon acts on is OB + LB):
  OL     inverter is connected to the grid (ongrid_reg=1) and grid voltage >= grid_min_v
  OB     otherwise (grid lost, or the inverter is detached from the grid and runs from the battery)
  LB     BMS/board reports hard-low (HARD_LOW / CELL_HARD flag) or SOC <= lb_soc
  CHRG   on line and charge current >= chg_min_a;   DISCHRG  on battery
If the board is unreachable or the inverter data is stale the file is NOT changed (no false shutdowns);
after `stale_alarm_s` it only adds `ups.alarm`. Nothing here switches anything.

Config JSON (default /etc/gs-nut-feeder.json, not in the repository; example: etc/nut/gs-nut-feeder.json.example):
  board_url (required), out (/var/lib/nut/gs-ups.dev), interval_s (10), grid_min_v (180), lb_soc (10),
  chg_min_a (0.3), rated_w (0 = do not publish ups.load), stale_alarm_s (300)
Usage: gs_nut_feeder.py [--config F] [--once] [--print]
"""
from __future__ import annotations

import argparse
import json
import os
import sys
import time
import urllib.request

DEFAULT_CONF = '/etc/gs-nut-feeder.json'
DEFAULTS = {'out': '/var/lib/nut/gs-ups.dev', 'interval_s': 10, 'grid_min_v': 180.0, 'lb_soc': 10,
            'chg_min_a': 0.3, 'rated_w': 0, 'stale_alarm_s': 300}
LB_FLAGS = ('HARD_LOW', 'CELL_HARD')


def fetch_state(board_url, timeout=8):
    with urllib.request.urlopen(board_url.rstrip('/') + '/api/state', timeout=timeout) as f:
        return json.loads(f.read().decode('utf-8', 'replace'))


def _num(v):
    return float(v) if isinstance(v, (int, float)) and not isinstance(v, bool) else None


def compute(st, conf):
    """dict of NUT variables from a board /api/state, or None when the inverter data is not trustworthy."""
    if not isinstance(st, dict) or st.get('rt_ok') is not True or not isinstance(st.get('voltages'), dict):
        return None
    v = st['voltages']
    grid, pack = _num(v.get('grid')), _num(v.get('battery'))
    if grid is None or pack is None:
        return None
    chg_a = _num(st.get('charge_a')) or 0.0
    load_w = _num(st.get('load_w')) or 0.0
    bat = st.get('battery') if isinstance(st.get('battery'), dict) else {}
    soc = _num(bat.get('soc'))
    flags = str((st.get('decision') or {}).get('flags', '')).split(',')
    on_line = st.get('ongrid_reg') == 1 and grid >= conf['grid_min_v']
    low = any(f in flags for f in LB_FLAGS) or (soc is not None and soc <= conf['lb_soc'])
    status = ['OL'] if on_line else ['OB']
    if on_line and chg_a >= conf['chg_min_a']:
        status.append('CHRG')
    if not on_line:
        status.append('DISCHRG')
    if low:
        status.append('LB')
    out = {
        'device.mfr': 'MUST', 'device.model': 'PV1800 + grid-supervisor', 'device.type': 'ups',
        'ups.mfr': 'MUST', 'ups.model': 'PV1800', 'ups.type': 'offline',
        'ups.status': ' '.join(status),
        'ups.realpower': '%d' % round(load_w),
        'input.voltage': '%.1f' % grid,
        'battery.voltage': '%.2f' % pack,
        'battery.charge.low': '%d' % conf['lb_soc'],
        'battery.type': 'LiFePO4',
    }
    if soc is not None:
        out['battery.charge'] = '%d' % round(soc)
    if conf['rated_w'] > 0:
        out['ups.load'] = '%d' % max(0, min(100, round(100.0 * load_w / conf['rated_w'])))
    if chg_a:
        out['battery.current'] = '%.1f' % chg_a
    if bat.get('cell_min_mv') is not None:
        out['battery.cell.min.mv'] = '%d' % bat['cell_min_mv']      # custom (not standard) variable
    return out


def render(vars_):
    """dummy-ups definition file text (`name: value` lines, no TIMER = static)."""
    return ''.join('%s: %s\n' % (k, vars_[k]) for k in sorted(vars_))


def write_atomic(path, text):
    d = os.path.dirname(path) or '.'
    os.makedirs(d, exist_ok=True)
    tmp = path + '.tmp'
    with open(tmp, 'w', encoding='utf-8') as f:
        f.write(text)
    os.replace(tmp, path)


def with_alarm(text, alarm):
    lines = [l for l in text.splitlines() if not l.startswith('ups.alarm:')]
    if alarm:
        lines.append('ups.alarm: %s' % alarm)
    return '\n'.join(sorted(lines)) + '\n'


def load_conf(path):
    conf = dict(DEFAULTS)
    try:
        with open(path, encoding='utf-8') as f:
            conf.update({k: v for k, v in json.load(f).items() if not k.startswith('_')})
    except FileNotFoundError:
        pass
    return conf


def step(conf, state_fn, last_ok, now, last_text):
    """One cycle. Returns (last_ok_time, last_text). Pure except for the file write."""
    try:
        vars_ = compute(state_fn(), conf)
    except Exception as e:                       # network / JSON problems: keep the last good picture
        sys.stderr.write('board read failed: %s\n' % e)
        vars_ = None
    if vars_ is not None:
        last_text = render(vars_)
        write_atomic(conf['out'], last_text)
        return now, last_text
    if last_text and now - last_ok >= conf['stale_alarm_s']:
        write_atomic(conf['out'], with_alarm(last_text, 'board data stale %d s' % (now - last_ok)))
    return last_ok, last_text


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    ap.add_argument('--config', default=DEFAULT_CONF)
    ap.add_argument('--once', action='store_true')
    ap.add_argument('--print', action='store_true', help='print the file instead of writing it (implies --once)')
    a = ap.parse_args(argv)
    conf = load_conf(a.config)
    if not conf.get('board_url'):
        raise SystemExit('board_url is not set in %s' % a.config)
    if a.print:
        v = compute(fetch_state(conf['board_url']), conf)
        sys.stdout.write(render(v) if v else '# no trustworthy data from the board\n')
        return 0 if v else 1
    last_ok, last_text = time.time(), ''
    while True:
        last_ok, last_text = step(conf, lambda: fetch_state(conf['board_url']), last_ok, time.time(), last_text)
        if a.once:
            return 0
        time.sleep(max(2, int(conf['interval_s'])))


if __name__ == '__main__':
    sys.exit(main())
