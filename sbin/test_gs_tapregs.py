#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""Тесты резервного источника регистров: RegCache/GuardRunner(regs_file) в eybond_tap и gs_tapregs. Запуск: python3 test_gs_tapregs.py"""
import json
import os
import sys
import tempfile
import unittest

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import eybond_tap as tap  # noqa: E402
import gs_tapregs  # noqa: E402
from test_eybond_guard import req, resp  # noqa: E402
from test_eybond_tap import MemLog  # noqa: E402


def snap(*blocks, t=1000.0):
    return {'t': t, 'blocks': [{'addr': a, 'count': len(r), 't': bt, 'regs': r} for a, r, bt in blocks]}


class AssembleTests(unittest.TestCase):
    def test_exact_and_subwindow(self):
        s = snap((100, list(range(10)), 1000.0))
        self.assertEqual(gs_tapregs.assemble(s, 100, 10, 1001.0), (list(range(10)), []))
        self.assertEqual(gs_tapregs.assemble(s, 103, 3, 1001.0), ([3, 4, 5], []))

    def test_two_blocks_and_gap_filled(self):
        s = snap((100, [1, 2, 3, 4, 5], 1000.0), (106, [7, 8, 9, 10], 1000.0))
        w, miss = gs_tapregs.assemble(s, 100, 10, 1001.0, min_cover=0.8)
        self.assertEqual(w, [1, 2, 3, 4, 5, 0, 7, 8, 9, 10])
        self.assertEqual(miss, [105])

    def test_first_missing_or_low_coverage(self):
        s = snap((105, [1, 2, 3], 1000.0))
        with self.assertRaises(LookupError):
            gs_tapregs.assemble(s, 100, 10, 1001.0)
        s = snap((100, [1, 2], 1000.0))
        with self.assertRaises(LookupError):
            gs_tapregs.assemble(s, 100, 10, 1001.0)

    def test_stale_ignored(self):
        s = snap((100, [1, 2, 3], 1000.0))
        with self.assertRaises(LookupError):
            gs_tapregs.assemble(s, 100, 3, 1000.0 + 400, max_age=300)

    def test_fill_constant(self):
        s = snap((20101, [5] * 10, 1000.0))
        w, miss = gs_tapregs.assemble(s, 20101, 11, 1001.0, min_cover=0.5, fill={20111: 4})
        self.assertEqual(w[-1], 4)
        self.assertEqual(miss, [])

    def test_newer_block_wins(self):
        s = snap((100, [1, 1, 1], 900.0), (100, [2, 2, 2], 1000.0))
        self.assertEqual(gs_tapregs.assemble(s, 100, 3, 1001.0)[0], [2, 2, 2])

    def test_load_missing_or_garbage(self):
        d = tempfile.mkdtemp()
        self.assertIsNone(gs_tapregs.load(os.path.join(d, 'none.json')))
        p = os.path.join(d, 'bad.json')
        with open(p, 'w') as f:
            f.write('{oops')
        self.assertIsNone(gs_tapregs.load(p))


class RegCacheTests(unittest.TestCase):
    def test_flush_roundtrip(self):
        d = tempfile.mkdtemp()
        p = os.path.join(d, 'sub', 'regs.json')
        T = [1000.0]
        c = tap.RegCache(p, lambda: T[0])
        self.assertFalse(c.flush())
        c.put(20101, [1, 2, 3])
        T[0] = 1002.0
        c.put(15201, [9])
        self.assertTrue(c.flush())
        self.assertFalse(c.flush())
        s = gs_tapregs.load(p)
        self.assertEqual(s['t'], 1002.0)
        self.assertEqual([b['addr'] for b in s['blocks']], [15201, 20101])
        self.assertEqual(gs_tapregs.assemble(s, 20101, 3, 1003.0)[0], [1, 2, 3])


class RunnerTests(unittest.TestCase):
    def test_runner_collects_without_tracker_or_guard(self):
        d = tempfile.mkdtemp()
        p = os.path.join(d, 'regs.json')
        cfg = dict(tap.CONFIG_DEFAULTS, host='h', regs_file=p, guard=False, tracker=False)
        log = MemLog()
        r = tap.GuardRunner(cfg, log, dry_run=True, notify=False)
        r.guard.feed(0, 1, 'S->C', req(20101, 3))
        r.guard.feed(0, 1, 'C->S', resp([7, 8, 9]))
        self.assertTrue(r.regs.flush())
        s = gs_tapregs.load(p)
        self.assertEqual(gs_tapregs.assemble(s, 20101, 3, s['t'])[0], [7, 8, 9])

    def test_config_accepts_regs_file(self):
        cfg, unknown = tap.validate_config({'regs_file': '/run/eybond-tap/regs.json'})
        self.assertEqual(cfg['regs_file'], '/run/eybond-tap/regs.json')
        self.assertNotIn('regs_file', unknown)
        cfg, _ = tap.validate_config({'regs_file': None})
        self.assertIsNone(cfg.get('regs_file'))


if __name__ == '__main__':
    unittest.main()
