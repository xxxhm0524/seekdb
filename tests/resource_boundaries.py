#!/usr/bin/env python3
"""Assert scans don't pin the entire heap under a small page-cache budget."""
import argparse
from contextlib import ExitStack
import ctypes
import json
import os
from pathlib import Path
import sys
import tempfile
import time

sys.path.insert(0, os.path.dirname(__file__))
from protocol_client import Client


def peak_bytes(pid):
    if os.name != "nt":
        for line in Path('/proc/%d/status' % pid).read_text().splitlines():
            if line.startswith('VmHWM:'):
                return int(line.split()[1]) * 1024
        raise RuntimeError('VmHWM unavailable')
    from ctypes import wintypes
    class Counters(ctypes.Structure):
        _fields_ = [('cb', wintypes.DWORD), ('PageFaultCount', wintypes.DWORD)] + [
            (name, ctypes.c_size_t) for name in ('PeakWorkingSetSize', 'WorkingSetSize',
            'QuotaPeakPagedPoolUsage', 'QuotaPagedPoolUsage', 'QuotaPeakNonPagedPoolUsage',
            'QuotaNonPagedPoolUsage', 'PagefileUsage', 'PeakPagefileUsage')]
    kernel = ctypes.WinDLL('kernel32', use_last_error=True)
    kernel.OpenProcess.restype = wintypes.HANDLE
    kernel.CloseHandle.argtypes = [wintypes.HANDLE]
    handle = kernel.OpenProcess(0x410, False, pid)
    if not handle:
        raise ctypes.WinError(ctypes.get_last_error())
    try:
        counters = Counters()
        counters.cb = ctypes.sizeof(counters)
        get_memory = ctypes.WinDLL('psapi').GetProcessMemoryInfo
        get_memory.argtypes = [wintypes.HANDLE, ctypes.c_void_p, wintypes.DWORD]
        if not get_memory(handle, ctypes.byref(counters), counters.cb):
            raise ctypes.WinError(ctypes.get_last_error())
        return counters.PeakWorkingSetSize
    finally:
        kernel.CloseHandle(handle)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--rows', type=int, default=16000)
    parser.add_argument('--max-mb', type=int, default=48)
    args = parser.parse_args()
    with tempfile.TemporaryDirectory(prefix='mvd-resource-') as data, ExitStack() as cleanup:
        client = Client(datadir=data, extra=['--cache-mb=1'])
        cleanup.callback(client.close)
        def sql(text, **expected):
            response = client.request(text)
            assert response.get('ok') is True, response
            for field, value in expected.items():
                assert response.get(field) == value, response
            return response
        sql('CREATE TABLE t (id BIGINT PRIMARY KEY, s VARCHAR(2048), v VECTOR(2));')
        for start in range(0, args.rows, 100):
            ids = range(start, min(start + 100, args.rows))
            sql("INSERT INTO t VALUES " + ','.join("(%d,'x','[%d,0]')" % (i, i) for i in ids),
                affected_rows=len(ids))
        client.shutdown()
        disk = sum(p.stat().st_size for p in Path(data).glob('*'))
        client = Client(datadir=data, extra=['--cache-mb=1'])
        cleanup.callback(client.close)
        checks = [
            ('ordered scalar limit', 'SELECT id FROM t ORDER BY id DESC LIMIT 3', [[args.rows-1],[args.rows-2],[args.rows-3]]),
            ('distance with offset', "SELECT id FROM t ORDER BY L2_DISTANCE(v,'[0,0]') LIMIT 3 OFFSET 1", [[1],[2],[3]]),
        ]
        for name, text, expected in checks:
            start = time.perf_counter()
            sql(text, rows=expected)
            peak = peak_bytes(client.p.pid)
            print(json.dumps(dict(check=name, peak_mb=round(peak/2**20,2),
                disk_mb=round(disk/2**20,2), seconds=round(time.perf_counter()-start,3))), flush=True)
            assert peak <= args.max_mb*2**20, '%s pins heap: peak %.1f MiB > %d MiB' % (name, peak/2**20, args.max_mb)
        sql('BEGIN')
        sql("UPDATE t SET v='[-1,0]' WHERE id=%d" % (args.rows-1), affected_rows=1)
        sql("SELECT id FROM t ORDER BY L2_DISTANCE(v,'[0,0]') LIMIT 3", rows=[[0],[1],[args.rows-1]])
        peak = peak_bytes(client.p.pid)
        print(json.dumps(dict(check='transaction TopK', peak_mb=round(peak/2**20,2))), flush=True)
        assert peak <= args.max_mb*2**20, 'transaction scan exceeds memory budget'
        sql('ROLLBACK')
        sql("UPDATE t SET s='changed'", affected_rows=args.rows)
        sql('SELECT s FROM t WHERE id=%d' % (args.rows-1), rows=[['changed']])
        peak = peak_bytes(client.p.pid)
        print(json.dumps(dict(check='whole-table UPDATE', peak_mb=round(peak/2**20,2))), flush=True)
        assert peak <= args.max_mb*2**20, 'UPDATE retains complete copies of wide rows'
        # Count every updated value, rather than checking just the first/last
        # rows and then deleting by identity alone.
        sql("DELETE FROM t WHERE s='changed'", affected_rows=args.rows)
        sql('SELECT id FROM t', rows=[])
        peak = peak_bytes(client.p.pid)
        print(json.dumps(dict(check='whole-table DELETE', peak_mb=round(peak/2**20,2))), flush=True)
        assert peak <= args.max_mb*2**20, 'DELETE/COMMIT exceeds memory budget'
        client.shutdown()
    print('resource boundaries: PASS')


if __name__ == '__main__':
    main()
