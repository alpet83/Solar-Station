"""Registers seen by eybond_tap in the datalogger's own Modbus polling -> windows for pv1800_ctrl.py.

eybond_tap (config key "regs_file") writes the latest response per block start address:
  {"t": <newest epoch>, "blocks": [{"addr": 20101, "count": 32, "t": <epoch>, "regs": [...]}, ...]}
`assemble()` builds any (addr, count) window from those blocks. The datalogger does not poll every register
pv1800_ctrl reads (e.g. 20133-20141, 20144, 10113-10123), so a window may be incomplete: gaps are filled with 0 and
reported, but only if the first word is known and at least `min_cover` of the window is covered; otherwise LookupError.
Blocks older than `max_age` seconds are ignored (a silent logger must never look like fresh data).
"""
import json


def load(path):
    try:
        with open(path, encoding='utf-8') as f:
            snap = json.load(f)
        return snap if isinstance(snap, dict) and isinstance(snap.get('blocks'), list) else None
    except (OSError, ValueError):
        return None


def assemble(snap, addr, count, now, max_age=300.0, min_cover=0.85, fill=None):
    """-> (words, missing_addrs). fill = {addr: value} constants for registers the logger never reads."""
    words = [None] * count
    for b in sorted(snap.get('blocks', []), key=lambda b: b.get('t', 0)):      # oldest first: newer overwrite
        try:
            a0, regs, t = int(b['addr']), b['regs'], float(b['t'])
        except (KeyError, TypeError, ValueError):
            continue
        if now - t > max_age:
            continue
        for a in range(max(addr, a0), min(addr + count, a0 + len(regs))):
            words[a - addr] = int(regs[a - a0])
    for a, v in (fill or {}).items():
        if addr <= a < addr + count and words[a - addr] is None:
            words[a - addr] = v
    missing = [addr + i for i, w in enumerate(words) if w is None]
    if words[0] is None:
        raise LookupError('first register %d not available (or stale)' % addr)
    if 1.0 - float(len(missing)) / count < min_cover:
        raise LookupError('%d@%d: only %d of %d registers available' % (count, addr, count - len(missing), count))
    return [w if w is not None else 0 for w in words], missing
