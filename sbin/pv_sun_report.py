#!/usr/bin/env python3
"""Daily PV report: when did solar charging really start/stop relative to sunrise/sunset.

Reads the day CSV that the grid-supervisor board writes to its SD card (GET /api/log?f=YYYYMMDD.csv, UTC
day, one row per 10 s), finds the on/off runs of the inverter's PV relay, and compares them with computed
sunrise/sunset and (optionally) Open-Meteo irradiance. Appends one row per day to a history CSV and, once
enough days are collected, prints a recommended "allow solar charging" window. One short digest goes to
the telebot (like batmon.php: POST $(cat /etc/telebot)/reg_event.php) - never one message per relay click.

A run shorter than `stable_s` is "unsure" (the relay clicking at dawn/dusk). Resolution = 10 s (CSV period).

Config (JSON, NOT in the repository; example: etc/pv-sun-report.json.example), default /etc/pv-sun-report.json:
  board_url   http://grid-supervisor.local      (required; alias from /etc/hosts; read-only API, no password)
  lat, lon    optional; default = fix from local gpsd (port gpsd_port, 2947; disable with "gpsd": false),
              else the representative city of the system time zone from zone.tab.
              WARNING: the host may be set to another zone than the site (e.g. Europe/London) - then the
              coordinates are wrong. The chosen coordinates and their source are always logged/reported.
  tz          optional IANA name used to print local times (default: UTC)
  stable_s    600
  out_dir     /var/lib/pv-sun-report
  weather     true      (Open-Meteo, no key; failures are ignored)
  telebot     /etc/telebot   (empty string = no notification)
Usage: pv_sun_report.py [--config F] [--date YYYY-MM-DD] [--csv FILE] [--no-weather] [--no-notify] [--dry-run]
"""
from __future__ import annotations

import argparse
import csv
import io
import json
import math
import os
import socket
import statistics
import sys
import time
import urllib.parse
import urllib.request
from datetime import date, datetime, timedelta, timezone

DEFAULT_CONF = '/etc/pv-sun-report.json'
SAMPLE_S = 10
MAX_DT_S = 60            # longest gap integrated into energy
HIST_FIELDS = ['date', 'sunrise', 'sunset', 'first_on', 'first_stable', 'last_stable_end', 'last_off', 'runs', 'short_runs',
               'max_chg_w', 'wh', 'pv_v_start', 'chg_w_start', 'ghi_start', 'min_ghi_stable', 'start_off_min', 'end_off_min']


# ---------------------------------------------------------------- sun & location

def sun_times(d: date, lat: float, lon: float):
    """(sunrise_epoch, sunset_epoch) UTC for the given UTC date, or None in polar day/night. Accuracy ~1 min."""
    jd = d.toordinal() + 1721424.5 + 0.5
    n = math.ceil(jd - 2451545.0 + 0.0008)
    jstar = n - lon / 360.0                 # lon: east positive
    m = math.radians((357.5291 + 0.98560028 * jstar) % 360)
    c = 1.9148 * math.sin(m) + 0.02 * math.sin(2 * m) + 0.0003 * math.sin(3 * m)
    lam = math.radians((math.degrees(m) + c + 180 + 102.9372) % 360)
    transit = 2451545.0 + jstar + 0.0053 * math.sin(m) - 0.0069 * math.sin(2 * lam)
    sin_dec = math.sin(lam) * math.sin(math.radians(23.4397))
    cos_dec = math.sqrt(1 - sin_dec ** 2)
    phi = math.radians(lat)
    cos_w = (math.sin(math.radians(-0.833)) - math.sin(phi) * sin_dec) / (math.cos(phi) * cos_dec)
    if cos_w < -1 or cos_w > 1:
        return None
    w = math.degrees(math.acos(cos_w))
    to_epoch = lambda j: int(round((j - 2440587.5) * 86400))
    return to_epoch(transit - w / 360.0), to_epoch(transit + w / 360.0)


def parse_iso6709(coord: str):
    """'+3510+03322' / '+351000+0332200' -> (lat, lon) degrees."""
    i = max(coord.rfind('+'), coord.rfind('-'))
    la, lo = coord[:i], coord[i:]

    def conv(s, deg_digits):
        sign = -1 if s[0] == '-' else 1
        s = s[1:]
        deg = int(s[:deg_digits]); minute = int(s[deg_digits:deg_digits + 2]); sec = int(s[deg_digits + 2:deg_digits + 4] or 0)
        return sign * (deg + minute / 60.0 + sec / 3600.0)
    return conv(la, 2), conv(lo, 3)


def zone_coords(tzname: str, tab_paths=('/usr/share/zoneinfo/zone1970.tab', '/usr/share/zoneinfo/zone.tab')):
    """Coordinates of the representative city of an IANA zone from zone.tab, or None."""
    for p in tab_paths:
        try:
            with open(p, encoding='utf-8') as f:
                for line in f:
                    if line.startswith('#'):
                        continue
                    parts = line.rstrip('\n').split('\t')
                    if len(parts) >= 3 and tzname in parts[2].split(',') or (len(parts) >= 3 and parts[2] == tzname):
                        return parse_iso6709(parts[1])
        except OSError:
            continue
    return None


def system_tz():
    try:
        with open('/etc/timezone') as f:
            return f.read().strip()
    except OSError:
        pass
    try:
        link = os.path.realpath('/etc/localtime')
        if '/zoneinfo/' in link:
            return link.split('/zoneinfo/', 1)[1]
    except OSError:
        pass
    return ''


def gpsd_fix(host='127.0.0.1', port=2947, timeout=6.0):
    """(lat, lon) from a local gpsd (JSON TPV with a 2D/3D fix) or None. No dependencies."""
    try:
        with socket.create_connection((host, port), timeout=timeout) as s:
            s.settimeout(timeout)
            s.sendall(b'?WATCH={"enable":true,"json":true};\n')
            end, buf = time.time() + timeout, b''
            while time.time() < end:
                chunk = s.recv(4096)
                if not chunk:
                    break
                buf += chunk
                lines = buf.split(b'\n')
                buf = lines.pop()
                for ln in lines:
                    try:
                        j = json.loads(ln)
                    except ValueError:
                        continue
                    if j.get('class') == 'TPV' and j.get('mode', 0) >= 2 and 'lat' in j and 'lon' in j:
                        return float(j['lat']), float(j['lon'])
    except (OSError, ValueError):
        pass
    return None


def location(conf: dict):
    """(lat, lon, source): config, else local gpsd fix, else the system time zone's representative city."""
    if conf.get('lat') is not None and conf.get('lon') is not None:
        return float(conf['lat']), float(conf['lon']), 'config'
    if conf.get('gpsd', True):
        g = gpsd_fix(port=int(conf.get('gpsd_port', 2947)))
        if g:
            return g[0], g[1], 'gpsd'
    tz = conf.get('system_tz') or system_tz()
    c = zone_coords(tz) if tz else None
    if c:
        return c[0], c[1], 'system tz %s' % tz
    raise SystemExit('no coordinates: set lat/lon in %s (system time zone %r not found in zone.tab)' % (DEFAULT_CONF, tz))


# ---------------------------------------------------------------- CSV analysis

def parse_csv(text: str):
    """Rows of the board's day CSV -> list of dicts with numeric fields (None for empty)."""
    rows = []
    for r in csv.DictReader(io.StringIO(text)):
        try:
            ep = int(r['epoch'])
        except (KeyError, ValueError, TypeError):
            continue
        if ep <= 0:
            continue

        def num(k):
            v = r.get(k)
            if v in (None, ''):
                return None
            try:
                return float(v)
            except ValueError:
                return None
        rows.append({'t': ep, 'pack_v': num('pack_v'), 'chg_a': num('chg_a'), 'chg_w': num('chg_w'), 'pv_v': num('pv_v'), 'pv_relay': num('pv_relay')})
    rows.sort(key=lambda r: r['t'])
    return rows


def _is_on(r, min_w=10.0):
    if r['pv_relay'] is not None:
        return r['pv_relay'] >= 0.5
    return (r['chg_w'] or 0.0) >= min_w


def find_runs(rows, stable_s=600):
    """On-runs of the PV relay: [{start,end,dur,stable,pv_v,chg_w,max_w,open}] (end = first 'off' sample)."""
    runs, cur = [], None
    for r in rows:
        on = _is_on(r)
        if on and cur is None:
            cur = {'start': r['t'], 'pv_v': r['pv_v'], 'chg_w': r['chg_w'], 'max_w': r['chg_w'] or 0.0}
        elif on:
            cur['max_w'] = max(cur['max_w'], r['chg_w'] or 0.0)
        elif cur is not None:
            cur['end'] = r['t']; cur['open'] = False
            runs.append(cur); cur = None
    if cur is not None:
        cur['end'] = rows[-1]['t']; cur['open'] = True
        runs.append(cur)
    for x in runs:
        x['dur'] = x['end'] - x['start']
        x['stable'] = x['dur'] >= stable_s
    return runs


def energy_wh(rows):
    wh = 0.0
    for a, b in zip(rows, rows[1:]):
        dt = min(b['t'] - a['t'], MAX_DT_S)
        if dt > 0 and a['chg_w']:
            wh += a['chg_w'] * dt / 3600.0
    return wh


def summarize(day: date, rows, stable_s, sun, weather=None):
    """One history row (dict with HIST_FIELDS) for the day."""
    runs = find_runs(rows, stable_s) if rows else []
    stable = [x for x in runs if x['stable']]
    sunrise, sunset = sun if sun else (0, 0)
    s = {k: '' for k in HIST_FIELDS}
    s.update(date=day.isoformat(), sunrise=sunrise, sunset=sunset, runs=len(runs), short_runs=len([x for x in runs if not x['stable']]),
             max_chg_w=int(max((r['chg_w'] or 0 for r in rows), default=0)), wh=round(energy_wh(rows), 1))
    if runs:
        s['first_on'] = runs[0]['start']; s['last_off'] = runs[-1]['end']
    if stable:
        s['first_stable'] = stable[0]['start']; s['last_stable_end'] = stable[-1]['end']
        s['pv_v_start'] = stable[0]['pv_v'] if stable[0]['pv_v'] is not None else ''
        s['chg_w_start'] = stable[0]['chg_w'] if stable[0]['chg_w'] is not None else ''
        if sunrise:
            s['start_off_min'] = round((stable[0]['start'] - sunrise) / 60.0, 1)
            s['end_off_min'] = round((sunset - stable[-1]['end']) / 60.0, 1)
        if weather:
            s['ghi_start'] = weather_at(weather, stable[0]['start'])
            vals = [weather_at(weather, x['start']) for x in stable]
            vals = [v for v in vals if v != '']
            s['min_ghi_stable'] = min(vals) if vals else ''
    return s


# ---------------------------------------------------------------- weather (Open-Meteo, optional)

def fetch_weather(lat, lon, day: date, timeout=15):
    """{hour_epoch: shortwave_radiation W/m2} for the UTC day or None. No API key; any failure -> None."""
    url = ('https://api.open-meteo.com/v1/forecast?latitude=%.4f&longitude=%.4f&hourly=shortwave_radiation,cloud_cover'
           '&past_days=7&forecast_days=1&timezone=UTC' % (lat, lon))
    try:
        with urllib.request.urlopen(url, timeout=timeout) as f:
            j = json.load(f)
        out = {}
        for t, g in zip(j['hourly']['time'], j['hourly']['shortwave_radiation']):
            ep = int(datetime.strptime(t, '%Y-%m-%dT%H:%M').replace(tzinfo=timezone.utc).timestamp())
            if g is not None:
                out[ep] = g
        return out or None
    except Exception as e:                      # network/format problems must not break the report
        sys.stderr.write('weather unavailable: %s\n' % e)
        return None


def weather_at(weather, epoch):
    """Irradiance of the hour containing epoch (W/m2) or ''."""
    h = epoch - epoch % 3600
    return weather.get(h, '') if weather else ''


# ---------------------------------------------------------------- history & recommendation

def update_history(path, row):
    rows = []
    if os.path.exists(path):
        with open(path, newline='') as f:
            rows = [r for r in csv.DictReader(f) if r.get('date') != row['date']]
    rows.append({k: row.get(k, '') for k in HIST_FIELDS})
    rows.sort(key=lambda r: r['date'])
    tmp = path + '.tmp'
    with open(tmp, 'w', newline='') as f:
        w = csv.DictWriter(f, fieldnames=HIST_FIELDS)
        w.writeheader(); w.writerows(rows)
    os.replace(tmp, path)
    return rows


def _pct(vals, p):
    vals = sorted(vals)
    k = (len(vals) - 1) * p / 100.0
    lo, hi = int(math.floor(k)), int(math.ceil(k))
    return vals[lo] + (vals[hi] - vals[lo]) * (k - lo)


def recommend(rows, last_n=14, min_days=5):
    """Window from the last days with stable charging: (start_after_sunrise_min, stop_before_sunset_min, days) or None.
    Conservative: 80th percentile of the observed offsets (charging was stable at least that late/early on 80 % of days)."""
    st, en = [], []
    for r in rows[-last_n:]:
        try:
            st.append(float(r['start_off_min'])); en.append(float(r['end_off_min']))
        except (KeyError, ValueError, TypeError):
            continue
    if len(st) < min_days:
        return None
    return round(_pct(st, 80)), round(_pct(en, 80)), len(st)


# ---------------------------------------------------------------- report text

def fmt_t(epoch, tz):
    if not epoch:
        return '--:--'
    return datetime.fromtimestamp(int(epoch), tz).strftime('%H:%M')


def report_text(row, rec, loc, tzname, tz):
    d = row['date']
    lines = ['PV %s (%s): рассвет %s, закат %s' % (d, tzname, fmt_t(row['sunrise'], tz), fmt_t(row['sunset'], tz))]
    if row['runs'] == 0:
        lines.append('PV не включалась (максимум %s Вт)' % row['max_chg_w'])
    else:
        lines.append('включений %s, из них коротких (<10 мин) %s; первое %s, последнее выключение %s' %
                     (row['runs'], row['short_runs'], fmt_t(row['first_on'], tz), fmt_t(row['last_off'], tz)))
        if row['first_stable'] != '':
            lines.append('устойчивый заряд %s–%s (через %s мин после рассвета, за %s мин до заката); PV %s В / %s Вт на старте' %
                         (fmt_t(row['first_stable'], tz), fmt_t(row['last_stable_end'], tz), row['start_off_min'], row['end_off_min'],
                          row['pv_v_start'], row['chg_w_start']))
        lines.append('энергия %s Вт·ч, максимум %s Вт' % (row['wh'], row['max_chg_w']))
        if row['ghi_start'] != '':
            lines.append('освещённость на старте %s Вт/м², минимум за устойчивые включения %s' % (row['ghi_start'], row['min_ghi_stable']))
    if rec:
        lines.append('окно по %d дням: разрешать заряд с рассвета+%d мин до заката−%d мин' % (rec[2], rec[0], rec[1]))
    lines.append('координаты %.3f, %.3f (%s)' % (loc[0], loc[1], loc[2]))
    return '\n'.join(lines)


def send_telebot(text, path, tag='PV', value=0):
    try:
        with open(path) as f:
            srv = f.read().strip()
    except OSError as e:
        sys.stderr.write('telebot not configured: %s\n' % e)
        return False
    url = '%s/reg_event.php?tag=%s&host=%s&value=%s&flags=0' % (srv, urllib.parse.quote(tag), urllib.parse.quote(socket.gethostname()), value)
    data = urllib.parse.urlencode({'event': text.replace('\n', '\\n')}).encode()
    try:
        with urllib.request.urlopen(urllib.request.Request(url, data=data), timeout=15) as f:
            res = f.read().decode('utf-8', 'replace')
        return '#ERROR' not in res
    except Exception as e:
        sys.stderr.write('telebot send failed: %s\n' % e)
        return False


# ---------------------------------------------------------------- main

def load_conf(path):
    try:
        with open(path) as f:
            return json.load(f)
    except FileNotFoundError:
        return {}


def fetch_day_csv(board_url, day: date, timeout=30):
    url = '%s/api/log?f=%s.csv' % (board_url.rstrip('/'), day.strftime('%Y%m%d'))
    with urllib.request.urlopen(url, timeout=timeout) as f:
        return f.read().decode('utf-8', 'replace')


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    ap.add_argument('--config', default=DEFAULT_CONF)
    ap.add_argument('--date', help='UTC date YYYY-MM-DD (default: today UTC)')
    ap.add_argument('--csv', help='read this CSV file instead of the board')
    ap.add_argument('--no-weather', action='store_true')
    ap.add_argument('--no-notify', action='store_true')
    ap.add_argument('--dry-run', action='store_true', help='print only: no history file, no notification')
    a = ap.parse_args(argv)
    conf = load_conf(a.config)
    day = date.fromisoformat(a.date) if a.date else datetime.now(timezone.utc).date()
    lat, lon, src = location(conf)
    tzname = conf.get('tz', 'UTC')
    try:
        from zoneinfo import ZoneInfo
        tz = ZoneInfo(tzname)
    except Exception:
        tz, tzname = timezone.utc, 'UTC'
    if a.csv:
        with open(a.csv, encoding='utf-8') as f:
            text = f.read()
    else:
        if not conf.get('board_url'):
            raise SystemExit('board_url is not set in %s' % a.config)
        text = fetch_day_csv(conf['board_url'], day)
    rows = parse_csv(text)
    weather = None if (a.no_weather or not conf.get('weather', True)) else fetch_weather(lat, lon, day)
    row = summarize(day, rows, int(conf.get('stable_s', 600)), sun_times(day, lat, lon), weather)
    out_dir = conf.get('out_dir', '/var/lib/pv-sun-report')
    hist = None
    if not a.dry_run:
        os.makedirs(out_dir, exist_ok=True)
        hist = update_history(os.path.join(out_dir, 'days.csv'), row)
    rec = recommend(hist) if hist else None
    text_out = report_text(row, rec, (lat, lon, src), tzname, tz)
    print(text_out)
    if not a.dry_run and not a.no_notify and conf.get('telebot', '/etc/telebot'):
        send_telebot(text_out, conf.get('telebot', '/etc/telebot'), value=row['short_runs'])
    return 0


if __name__ == '__main__':
    sys.exit(main())
