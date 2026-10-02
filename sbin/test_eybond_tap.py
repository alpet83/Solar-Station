#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""Mock-тест eybond_tap: поддельные "облако" и "стик" на loopback. Запуск: python3 test_eybond_tap.py"""
import asyncio
import io
import os
import sys
import unittest

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import eybond_tap as tap  # noqa: E402


class MemLog(tap.Log):
    def __init__(self):
        self.fh = io.StringIO()


def mkargs(upstream_port, gap=0.2):
    return tap.build_parser().parse_args(
        ['--listen', '127.0.0.1:0', '--upstream', '127.0.0.1:%d' % upstream_port,
         '--burst-gap', str(gap)])


async def start_tap(args, log):
    stop, fut = asyncio.Event(), asyncio.get_running_loop().create_future()
    task = asyncio.ensure_future(tap.run(args, log, stop, ready=fut.set_result))
    return stop, task, await asyncio.wait_for(fut, 5)


class TapTest(unittest.IsolatedAsyncioTestCase):
    async def test_passthrough_and_bursts(self):
        got = []
        c2s = bytes(range(256)) * 3
        s2c1, s2c2 = b'\x00\x01\x00\x02' + b'A' * 100, b'\xff' * 10

        async def cloud(r, w):
            got.append(await r.readexactly(len(c2s)))
            w.write(s2c1)
            await w.drain()
            await asyncio.sleep(0.6)  # > burst-gap -> новый burst
            w.write(s2c2)
            await w.drain()
            await asyncio.sleep(0.5)
            w.close()

        srv = await asyncio.start_server(cloud, '127.0.0.1', 0)
        up = srv.sockets[0].getsockname()[1]
        log = MemLog()
        stop, task, port = await start_tap(mkargs(up), log)
        r, w = await asyncio.open_connection('127.0.0.1', port)
        w.write(c2s)
        await w.drain()
        data = await asyncio.wait_for(r.read(), 5)  # до EOF от облака
        self.assertEqual(got[0], c2s)
        self.assertEqual(data, s2c1 + s2c2)
        w.close()
        await asyncio.sleep(0.3)
        stop.set()
        await task
        srv.close()
        out = log.fh.getvalue()
        self.assertIn('C->S len=768', out)
        self.assertIn('S->C len=104', out)
        bursts = [l for l in out.splitlines() if 'BURST ' in l]
        self.assertEqual(len(bursts), 2, out)
        self.assertIn('chunks=1 bytes=104', bursts[0])
        self.assertIn('since_prev_start=n/a', bursts[0])
        self.assertNotIn('n/a', bursts[1])

    async def test_upstream_down_closes_client(self):
        s = await asyncio.start_server(lambda r, w: None, '127.0.0.1', 0)
        dead = s.sockets[0].getsockname()[1]
        s.close()
        await s.wait_closed()  # порт закрыт -> connection refused
        log = MemLog()
        stop, task, port = await start_tap(mkargs(dead), log)
        r, w = await asyncio.open_connection('127.0.0.1', port)
        self.assertEqual(await asyncio.wait_for(r.read(), 5), b'')
        w.close()
        stop.set()
        await task
        self.assertIn('UPSTREAM-FAIL', log.fh.getvalue())


if __name__ == '__main__':
    unittest.main()
