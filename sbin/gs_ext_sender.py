#!/usr/bin/env python3
"""Send the Epever charge current to the grid-supervisor board (POST /api/ext).

The board cannot see the Epever controller, but its on/off-grid decision needs the total charge
power. This daemon pushes `battery_current` from /tmp/epever_dump.json every 20 s. The board
expires the value after 60 s, so on any problem this script simply stops sending (fail-safe:
less charge seen -> the board stays on grid).

Config: /etc/gs-supervisor.conf (NOT in the repository)
  line 1: base URL, e.g. http://192.0.2.10
  line 2: user:password           (admin; used only if there is no token)
  line 3: optional ext token      (>= 16 chars, sent as X-GS-Token; preferred)
"""
import base64
import json
import os
import sys
import time
import urllib.request

CONF = os.environ.get('GS_CONF', '/etc/gs-supervisor.conf')
SRC = os.environ.get('GS_EPEVER', '/tmp/epever_dump.json')
PERIOD_S = 20
MAX_FILE_AGE_S = 90      # file age by mtime: its internal `ts` is in local time, not UTC
MAX_CURRENT_A = 100.0    # above this the value is considered garbage


def load_conf(path=CONF):
    with open(path) as f:
        lines = [l.strip() for l in f.read().splitlines() if l.strip()]
    user, _, pwd = lines[1].partition(':')
    return {'url': lines[0].rstrip('/'), 'user': user, 'pass': pwd, 'token': lines[2] if len(lines) > 2 else ''}


def read_current(path=SRC, now=None):
    """Charge current in A, or None when the data is missing, stale or implausible."""
    now = time.time() if now is None else now
    try:
        if now - os.path.getmtime(path) > MAX_FILE_AGE_S:
            return None
        with open(path) as f:
            cur = float(json.load(f)['battery_current'])
    except (OSError, ValueError, KeyError, TypeError):
        return None
    if cur != cur or cur < 0 or cur > MAX_CURRENT_A:   # NaN, discharge, garbage
        return None
    return cur


def send(conf, cur):
    req = urllib.request.Request(conf['url'] + '/api/ext', data=('a=%.2f' % cur).encode())
    req.add_header('X-GS', '1')
    if conf.get('token'):
        req.add_header('X-GS-Token', conf['token'])
    else:
        tok = base64.b64encode(f"{conf['user']}:{conf['pass']}".encode()).decode()
        req.add_header('Authorization', 'Basic ' + tok)
    with urllib.request.urlopen(req, timeout=5) as r:
        return r.status == 200


def main():
    conf = load_conf()
    while True:
        cur = read_current()
        if cur is not None:
            try:
                send(conf, cur)
            except Exception as e:
                print(f"#WARN: send failed: {e}", file=sys.stderr, flush=True)
        time.sleep(PERIOD_S)


if __name__ == '__main__':
    main()
