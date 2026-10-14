#!/usr/bin/env python3
"""spp_health_monitor: self-health watchdog for the solar-station Raspberry Pi (rpi3-spp).

Loop (default every 30 s). Each check yields the set of *active problems*; the engine turns changes into
(1) an incident journal  <state_dir>/incidents.jsonl  (open / close / action / event records),
(2) alerts to the telebot (POST $(cat /etc/telebot)/reg_event.php?tag=..&host=..&value=0&flags=0, text in the
    body `event=`; value/flags are never empty - the server answers 500 on empty ones),
(3) automatic cures where one is known: USB controller failure -> /usr/sbin/usb_recovery.sh.
Alerts that cannot be delivered (network is down - on the Pi 3 eth0 sits behind the same USB hub!) wait in an
outbox and are re-sent after the cure. Nothing here ever touches the inverter or the board.

Checks: usb (dmesg error bursts + expected tty devices), svc (systemd units), board (grid-supervisor /api/state:
silent, rebooted, not in normal mode, safe mode, inverter data invalid, off-grid with grid present and low battery),
net (interface operstate), pi (reboot, under-voltage, temperature, free disk).
Config JSON (default /etc/spp-health-monitor.json, NOT in the repository; example etc/spp-health-monitor.json.example).
Usage: spp_health_monitor.py [--config F] [--once] [--dry-run] [--test-alert]
Python 3.9+, standard library only.
"""
from __future__ import annotations

import argparse
import json
import os
import re
import socket
import subprocess
import sys
import time
import urllib.parse
import urllib.request
from datetime import datetime, timezone

DEFAULT_CONF = '/etc/spp-health-monitor.json'
DEFAULTS = {
    'interval_s': 30, 'state_dir': '/var/lib/spp-health', 'telebot': '/etc/telebot', 'renotify_s': 3600,
    'services': [],
    'usb': {'enabled': True, 'window_s': 120, 'min_errors': 5, 'recovery_cmd': '/usr/sbin/usb_recovery.sh',
            'cooldown_s': 600, 'max_per_hour': 3, 'expect_devices': []},
    'board': {'url': '', 'silent_s': 300, 'grid_min_v': 180.0, 'offgrid_alert_v': 49.0, 'offgrid_s': 120},
    'net': {'ifaces': ['eth0']},
    'pi': {'temp_c': 80, 'disk_free_pct': 10},
}
USB_PATTERNS = [re.compile(p) for p in (
    r'failed to send control message: -\d+', r'failed to receive control message: -\d+',
    r'failed to read modem status: -\d+', r'ftdi_set_termios.*FAILED', r'failed to set flow control: -\d+',
    r'device descriptor read/\d+, error -?\d+', r'unable to enumerate USB device', r'xhci.*(died|Host halt)')]
DMESG_RE = re.compile(r'^\[\s*(\d+(?:\.\d+)?)\]\s*(.*)$')
OUTBOX_MAX, OUTBOX_TTL = 50, 86400


def merge(base, over):
    out = dict(base)
    for k, v in (over or {}).items():
        out[k] = merge(out[k], v) if isinstance(v, dict) and isinstance(out.get(k), dict) else v
    return out


# ---------------------------------------------------------------- real I/O (replaced by fakes in tests)

class IO:
    def now(self): return time.time()
    def uptime(self):
        with open('/proc/uptime') as f: return float(f.read().split()[0])
    def run(self, cmd, timeout=30):
        try:
            p = subprocess.run(cmd, shell=isinstance(cmd, str), capture_output=True, text=True, timeout=timeout)
            return p.returncode, (p.stdout or '') + (p.stderr or '')
        except Exception as e:
            return 127, str(e)
    def read(self, path):
        try:
            with open(path, encoding='utf-8', errors='replace') as f: return f.read()
        except OSError:
            return None
    def exists(self, path): return os.path.exists(path)
    def http_get(self, url, timeout=8):
        with urllib.request.urlopen(url, timeout=timeout) as f: return f.read().decode('utf-8', 'replace')
    def http_post(self, url, data, timeout=15):
        req = urllib.request.Request(url, data=urllib.parse.urlencode(data).encode())
        with urllib.request.urlopen(req, timeout=timeout) as f: return f.read().decode('utf-8', 'replace')
    def disk_free_pct(self, path='/'):
        s = os.statvfs(path); return 100.0 * s.f_bavail / max(1, s.f_blocks)
    def sleep(self, s): time.sleep(s)


# ---------------------------------------------------------------- parsing helpers

def parse_dmesg(text):
    """[(uptime_seconds, message)] from plain `dmesg` output."""
    out = []
    for ln in text.splitlines():
        m = DMESG_RE.match(ln)
        if m: out.append((float(m.group(1)), m.group(2)))
    return out


def usb_error_times(lines, after=0.0):
    return [t for t, msg in lines if t > after and any(p.search(msg) for p in USB_PATTERNS)]


# ---------------------------------------------------------------- the monitor

class Monitor:
    def __init__(self, conf, io, dry_run=False):
        self.c, self.io, self.dry = conf, io, dry_run
        self.host = socket.gethostname()
        self.open = {}              # kind -> {since,last,sev,msg}
        self.outbox = []            # [{t,sev,text}]
        self.usb_seen = 0.0         # last dmesg timestamp processed
        self.usb_ignore = 0.0       # ignore errors up to this uptime (after a recovery)
        self.recoveries = []        # epoch times
        self.board_ok_t = None
        self.board_uptime = None
        self.offgrid_since = None
        self.boot_id = None
        self.started = False
        self.load_state()

    # -- persistence
    def _path(self, name): return os.path.join(self.c['state_dir'], name)
    def load_state(self):
        raw = self.io.read(self._path('state.json'))
        if not raw: return
        try:
            s = json.loads(raw)
            self.open, self.outbox = s.get('open', {}), s.get('outbox', [])
            self.recoveries, self.boot_id = s.get('recoveries', []), s.get('boot_id')
        except ValueError:
            pass
    def save_state(self):
        if self.dry: return
        os.makedirs(self.c['state_dir'], exist_ok=True)
        tmp = self._path('state.json.tmp')
        with open(tmp, 'w', encoding='utf-8') as f:
            json.dump({'open': self.open, 'outbox': self.outbox, 'recoveries': self.recoveries, 'boot_id': self.boot_id}, f)
        os.replace(tmp, self._path('state.json'))

    # -- journal / alerts
    def journal(self, kind, **rec):
        rec.update(t=int(self.io.now()), iso=datetime.fromtimestamp(self.io.now(), timezone.utc).strftime('%Y-%m-%dT%H:%M:%SZ'), kind=kind)
        line = json.dumps(rec, ensure_ascii=False)
        if self.dry:
            print('JOURNAL', line); return
        os.makedirs(self.c['state_dir'], exist_ok=True)
        with open(self._path('incidents.jsonl'), 'a', encoding='utf-8') as f: f.write(line + '\n')

    def send(self, sev, text, t=None):
        base = (self.io.read(self.c['telebot']) or '').strip() if self.c.get('telebot') else ''
        if self.dry or not base:
            print('ALERT[%s] %s' % (sev, text)); return bool(self.dry)
        tag = {'alert': 'ALERT', 'warn': 'WARN'}.get(sev, 'REPORT')
        url = '%s/reg_event.php?tag=%s&host=%s&value=0&flags=0' % (base, tag, urllib.parse.quote(self.host))
        try:
            return '#ERROR' not in self.io.http_post(url, {'event': text.replace('\n', '\\n')})
        except Exception:
            return False

    def notify(self, sev, text):
        if not self.send(sev, text):
            self.outbox.append({'t': int(self.io.now()), 'sev': sev, 'text': text})
            del self.outbox[:-OUTBOX_MAX]

    def flush_outbox(self):
        now, keep = self.io.now(), []
        for m in self.outbox:
            if now - m['t'] > OUTBOX_TTL: continue
            late = int((now - m['t']) / 60)
            txt = m['text'] if late < 2 else '[%d мин назад] %s' % (late, m['text'])
            if not self.send(m['sev'], txt): keep.append(m)
        self.outbox = keep

    def event(self, kind, sev, msg):
        self.journal('event', what=kind, sev=sev, msg=msg); self.notify(sev, msg)

    def update(self, problems, ran):
        now = self.io.now()
        for kind, (sev, msg) in problems.items():
            st = self.open.get(kind)
            if not st:
                self.open[kind] = {'since': now, 'last': now, 'sev': sev, 'msg': msg}
                self.journal('open', what=kind, sev=sev, msg=msg); self.notify(sev, msg)
            elif now - st['last'] >= self.c['renotify_s']:
                st['last'] = now
                self.notify(sev, '%s (длится %d мин)' % (msg, (now - st['since']) / 60))
        for kind in list(self.open):
            if kind.split(':', 1)[0] in ran and kind not in problems:
                st = self.open.pop(kind)
                self.journal('close', what=kind, duration_s=int(now - st['since']))
                self.notify('info', 'OK: %s — устранено через %d мин' % (st['msg'].split(':')[0], (now - st['since']) / 60 + 0.5))

    # -- checks: each returns (problems, ran_prefix)
    def check_usb(self):
        u = self.c['usb']
        if not u['enabled']: return {}, None
        rc, out = self.io.run('dmesg')
        if rc != 0: return {}, None
        lines = parse_dmesg(out)
        up = self.io.uptime()
        if lines and lines[-1][0] < self.usb_seen: self.usb_seen = 0.0      # dmesg ring restarted
        times = usb_error_times(lines, self.usb_ignore)
        recent = [t for t in times if t >= up - u['window_s']]
        problems = {}
        if len(recent) >= u['min_errors']:
            msg = 'USB: %d ошибок за %d с (%s)' % (len(recent), u['window_s'], next((m for t, m in reversed(lines) if any(p.search(m) for p in USB_PATTERNS)), '')[:90])
            problems['usb:errors'] = ('alert', msg)
            self.try_recovery()
        for dev in u['expect_devices']:
            if not self.io.exists(dev):
                problems['usb:dev:' + dev] = ('alert', 'USB: устройство %s отсутствует' % dev)
        return problems, 'usb'

    def try_recovery(self):
        u, now = self.c['usb'], self.io.now()
        self.recoveries = [t for t in self.recoveries if now - t < 3600]
        if not u.get('recovery_cmd'): return
        if self.recoveries and now - self.recoveries[-1] < u['cooldown_s']: return
        if len(self.recoveries) >= u['max_per_hour']:
            if not self.open.get('usb:giveup'):
                self.open['usb:giveup'] = {'since': now, 'last': now, 'sev': 'alert', 'msg': 'USB: автолечение исчерпано (%d за час)' % u['max_per_hour']}
                self.journal('open', what='usb:giveup'); self.notify('alert', self.open['usb:giveup']['msg'])
            return
        self.recoveries.append(now)
        if self.dry:
            print('WOULD RUN', u['recovery_cmd']); return
        rc, out = self.io.run(u['recovery_cmd'], timeout=120)
        self.usb_ignore = self.io.uptime()
        self.journal('action', what='usb_recovery', cmd=u['recovery_cmd'], rc=rc, out=out.strip()[-300:])
        self.notify('warn', 'USB: запущен %s, код %s' % (u['recovery_cmd'], rc))

    def check_services(self):
        problems = {}
        for unit in self.c['services']:
            rc, out = self.io.run(['systemctl', 'is-active', unit])
            if rc != 0: problems['svc:' + unit] = ('warn', 'служба %s не активна (%s)' % (unit, out.strip() or 'нет ответа'))
        return problems, 'svc' if self.c['services'] else None

    def check_board(self):
        b = self.c['board']
        if not b['url']: return {}, None
        now, problems = self.io.now(), {}
        try:
            st = json.loads(self.io.http_get(b['url'].rstrip('/') + '/api/state'))
            self.board_ok_t = now
        except Exception as e:
            if self.board_ok_t is None: self.board_ok_t = now
            if now - self.board_ok_t >= b['silent_s']:
                problems['board:silent'] = ('alert', 'плата grid-supervisor молчит %d мин (%s)' % ((now - self.board_ok_t) / 60, str(e)[:60]))
            return problems, 'board'
        up = st.get('uptime_ms')
        if isinstance(up, (int, float)):
            if self.board_uptime is not None and up < self.board_uptime - 5000:
                reason = ''
                try:
                    ev = [l for l in self.io.http_get(b['url'].rstrip('/') + '/api/log?f=events.log').splitlines() if 'BOOT' in l]
                    reason = ev[-1].split(',', 1)[-1][:80] if ev else ''
                except Exception:
                    pass
                self.event('board_reboot', 'warn', 'плата grid-supervisor перезагрузилась: %s' % reason)
            self.board_uptime = up
        if st.get('mode') != 'normal': problems['board:mode'] = ('alert', 'плата не в режиме normal (mode=%s): защиты не работают' % st.get('mode'))
        if st.get('safe_mode'): problems['board:safe'] = ('alert', 'плата в safe-mode')
        if st.get('rt_ok') is not True: problems['board:rt'] = ('alert', 'плата не получает данные инвертора (rt_ok=false)')
        v = st.get('voltages') or {}
        grid, pack = v.get('grid'), v.get('battery')
        bad = (st.get('ongrid_reg') == 0 and isinstance(grid, (int, float)) and grid >= b['grid_min_v']
               and isinstance(pack, (int, float)) and pack <= b['offgrid_alert_v'])
        self.offgrid_since = (self.offgrid_since or now) if bad else None
        if bad and now - self.offgrid_since >= b['offgrid_s']:
            problems['board:offgrid'] = ('alert', 'инвертор отсоединён от сети при сети %.0f В и батарее %.1f В' % (grid, pack))
        return problems, 'board'

    def check_net(self):
        problems = {}
        for i in self.c['net']['ifaces']:
            s = (self.io.read('/sys/class/net/%s/operstate' % i) or '').strip()
            if s and s != 'up': problems['net:' + i] = ('warn', 'интерфейс %s: %s' % (i, s))
        return problems, 'net' if self.c['net']['ifaces'] else None

    def check_pi(self):
        p, problems = self.c['pi'], {}
        bid = (self.io.read('/proc/sys/kernel/random/boot_id') or '').strip()
        if bid and bid != self.boot_id:
            if self.boot_id is not None:
                self.event('pi_reboot', 'warn', 'малина %s перезагрузилась (uptime %d с)' % (self.host, self.io.uptime()))
            self.boot_id = bid
        rc, out = self.io.run('vcgencmd get_throttled')
        m = re.search(r'0x([0-9a-fA-F]+)', out) if rc == 0 else None
        if m and int(m.group(1), 16) & 0x1:
            problems['pi:undervolt'] = ('warn', 'малина: пониженное напряжение питания (get_throttled=0x%s)' % m.group(1))
        t = self.io.read('/sys/class/thermal/thermal_zone0/temp')
        if t and t.strip().isdigit() and int(t) / 1000.0 >= p['temp_c']:
            problems['pi:temp'] = ('warn', 'малина: температура %.0f °C' % (int(t) / 1000.0))
        try:
            free = self.io.disk_free_pct()
            if free < p['disk_free_pct']: problems['pi:disk'] = ('warn', 'малина: свободно на / только %.0f %%' % free)
        except OSError:
            pass
        return problems, 'pi'

    # -- main
    def step(self):
        problems, ran = {}, set()
        for chk in (self.check_usb, self.check_services, self.check_board, self.check_net, self.check_pi):
            try:
                pr, r = chk()
            except Exception as e:
                sys.stderr.write('%s failed: %r\n' % (chk.__name__, e)); continue
            problems.update(pr)
            if r: ran.add(r)
        if 'usb:giveup' in self.open and 'usb:errors' not in problems: ran.add('usb')
        if 'usb:giveup' in self.open and 'usb:errors' in problems: problems['usb:giveup'] = (self.open['usb:giveup']['sev'], self.open['usb:giveup']['msg'])
        self.update(problems, ran)
        self.flush_outbox()
        self.save_state()
        return problems


def load_conf(path):
    try:
        with open(path, encoding='utf-8') as f: return merge(DEFAULTS, json.load(f))
    except FileNotFoundError:
        return merge(DEFAULTS, {})


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    ap.add_argument('--config', default=DEFAULT_CONF)
    ap.add_argument('--once', action='store_true')
    ap.add_argument('--dry-run', action='store_true', help='print alerts/journal, do not send, write or run recovery (implies --once)')
    ap.add_argument('--test-alert', action='store_true', help='send one test alert through reg_event.php and exit')
    a = ap.parse_args(argv)
    conf, io = load_conf(a.config), IO()
    mon = Monitor(conf, io, dry_run=a.dry_run)
    if a.test_alert:
        ok = mon.send('info', 'тест spp_health_monitor на %s' % mon.host)
        print('sent' if ok else 'FAILED'); return 0 if ok else 1
    while True:
        pr = mon.step()
        if a.once or a.dry_run:
            print('active problems:', ', '.join(sorted(pr)) or 'none'); return 0
        io.sleep(max(5, int(conf['interval_s'])))


if __name__ == '__main__':
    sys.exit(main())
