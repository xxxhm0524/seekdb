#!/usr/bin/env python3
"""Avoid redundant wide-row reads during unfiltered mutations and full deletes."""
from contextlib import ExitStack
import ctypes
import json
import os
from pathlib import Path
import sys
import tempfile

sys.path.insert(0, str(Path(__file__).resolve().parent))
from protocol_client import Client


def read_bytes(pid):
    if os.name != 'nt':
        fields = dict(line.split(':', 1) for line in Path('/proc/%d/io' % pid).read_text().splitlines())
        return int(fields['rchar'])
    from ctypes import wintypes
    class Counters(ctypes.Structure):
        _fields_ = [(name, ctypes.c_ulonglong) for name in (
            'read_ops', 'write_ops', 'other_ops', 'read_bytes', 'write_bytes', 'other_bytes')]
    kernel = ctypes.WinDLL('kernel32', use_last_error=True)
    kernel.OpenProcess.restype = wintypes.HANDLE
    kernel.CloseHandle.argtypes = [wintypes.HANDLE]
    kernel.GetProcessIoCounters.argtypes = [wintypes.HANDLE, ctypes.POINTER(Counters)]
    handle = kernel.OpenProcess(0x1000, False, pid)
    if not handle:
        raise ctypes.WinError(ctypes.get_last_error())
    try:
        counters = Counters()
        if not kernel.GetProcessIoCounters(handle, ctypes.byref(counters)):
            raise ctypes.WinError(ctypes.get_last_error())
        return counters.read_bytes
    finally:
        kernel.CloseHandle(handle)


def main():
    with tempfile.TemporaryDirectory(prefix='mvd-mutation-io-') as data, ExitStack() as cleanup:
        client = Client(datadir=data, extra=['--cache-mb=1'])
        cleanup.callback(client.close)
        def sql(text, **expected):
            response = client.request(text)
            assert response.get('ok') is True, (text[:100], response)
            for field, value in expected.items():
                assert response.get(field) == value, (text[:100], response)
            return response
        rows = 5001  # Include both full pages and a partial final page.
        sql('CREATE TABLE t(id BIGINT PRIMARY KEY,s VARCHAR(2048),v VECTOR(2))')
        for start in range(0, rows, 100):
            stop = min(start + 100, rows)
            sql('INSERT INTO t VALUES ' + ','.join("(%d,'x','[%d,0]')" % (i, i)
                for i in range(start, stop)), affected_rows=stop-start)
        client.kill(); client.start()
        disk = sum(p.stat().st_size for p in Path(data).glob('t_*.dat'))
        # Additions, deletions and reinsertions in the overlay must be visited once.
        sql('BEGIN')
        sql('DELETE FROM t WHERE id=1', affected_rows=1)
        sql("INSERT INTO t VALUES (1,'r','[1,0]'),(6000,'r','[6000,0]')", affected_rows=2)
        before = read_bytes(client.p.pid)
        sql("UPDATE t SET s='changed'", affected_rows=rows+1)
        update_reads = read_bytes(client.p.pid)-before
        assert update_reads < 2*2**20, ('unfiltered UPDATE reread the heap', update_reads, disk)
        sql('SELECT id,s FROM t WHERE id>=4999 ORDER BY id', rows=[[4999, 'changed'], [5000, 'changed'], [6000, 'changed']])
        sql('ROLLBACK')
        sql('SELECT s FROM t WHERE id=1', rows=[['x']])
        sql('SELECT id FROM t WHERE id=6000', rows=[])
        sql('BEGIN')
        before = read_bytes(client.p.pid)
        sql('DELETE FROM t', affected_rows=rows)
        enumeration_reads = read_bytes(client.p.pid)-before
        assert enumeration_reads < 2*2**20, ('unfiltered DELETE reread the heap', enumeration_reads, disk)
        before = read_bytes(client.p.pid)
        sql('COMMIT')
        commit_reads = read_bytes(client.p.pid)-before
        # WAL replay still reads its page images. Fully replaced pages should
        # not also be read from the old heap; allow headers and boundary pages.
        assert commit_reads < disk*1.35+2*2**20, ('full delete read old page bodies', commit_reads, disk)
        client.kill(); client.start()
        sql('SELECT id FROM t', rows=[])
        # Partial deletion must preserve untouched rows on shared pages.
        sql("INSERT INTO t VALUES (7000,'keep','[7,0]'),(7001,'drop','[8,0]')", affected_rows=2)
        sql("DELETE FROM t WHERE s='drop'", affected_rows=1)
        client.kill(); client.start()
        sql('SELECT id,s,v FROM t', rows=[[7000, 'keep', [7, 0]]])
        print(json.dumps(dict(passed=True, heap_bytes=disk, update_reads=update_reads,
            delete_enumeration_reads=enumeration_reads, delete_commit_reads=commit_reads)))


if __name__ == '__main__':
    main()
