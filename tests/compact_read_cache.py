#!/usr/bin/env python3
"""Repeated wide scans reuse bounded cached bytes; mutations cannot reuse stale pages."""
import argparse
from contextlib import ExitStack
import json
from pathlib import Path
import sys
import tempfile

sys.path.insert(0,str(Path(__file__).resolve().parent))
from protocol_client import Client
from mutation_io import read_bytes
from resource_boundaries import peak_bytes

parser = argparse.ArgumentParser()
parser.add_argument('--rows', type=int, default=12000)
parser.add_argument('--max-mb', type=int, default=64)
parser.add_argument('--output', type=Path)
args = parser.parse_args()
report = dict(passed=False, rows=args.rows)
try:
    with tempfile.TemporaryDirectory(prefix='mvd-compact-read-') as data, ExitStack() as cleanup:
        client = Client(datadir=data,extra=['--cache-mb=1','--tx-memory-mb=1'])
        cleanup.callback(client.close)
        def sql(text,**expected):
            value = client.request(text)
            assert value.get('ok') is True,(text[:90],value)
            for k,v in expected.items():assert value.get(k) == v,(k,value)
            return value
        dense = '\U0001f600'*2048
        sql('CREATE TABLE t(id BIGINT PRIMARY KEY,s VARCHAR(2048),v VECTOR(2))')
        sql('BEGIN')
        for start in range(0,args.rows,100):
            stop = min(start+100,args.rows)
            sql('INSERT INTO t VALUES '+','.join("(%d,'%s','[%d,0]')" % (i,dense,i)
                for i in range(start,stop)),affected_rows=stop-start)
        sql('COMMIT')
        client.shutdown(); client.start()
        heap = (Path(data)/'t_1.dat').stat().st_size
        query = "SELECT id FROM t WHERE s='%s' ORDER BY id DESC LIMIT 3" % dense
        expected = [[i] for i in range(args.rows-1,args.rows-4,-1)]
        before = read_bytes(client.p.pid)
        sql(query, rows=expected)
        cold = read_bytes(client.p.pid)-before
        before = read_bytes(client.p.pid)
        sql(query, rows=expected)
        warm = read_bytes(client.p.pid)-before
        report.update(heap_bytes=heap,cold_read_bytes=cold,warm_read_bytes=warm)
        assert cold > heap//2,(cold,heap)
        assert warm < heap//16+1024*1024,('warm scan reread the heap',cold,warm,heap)
        sql("UPDATE t SET s='changed',v='[-1,0]' WHERE id=0",affected_rows=1)
        sql(query, rows=expected) # Evict the updated raw page before checking it.
        sql("SELECT id,s,v FROM t WHERE s='changed' ORDER BY id",rows=[[0,'changed',[-1,0]]])
        sql('BEGIN')
        sql("UPDATE t SET s='uncommitted' WHERE id=0",affected_rows=1)
        sql('ROLLBACK')
        sql('SELECT s FROM t WHERE id=0',rows=[['changed']])
        sql('DELETE FROM t WHERE id=0',affected_rows=1)
        sql("INSERT INTO t VALUES (0,'reused','[3,4]')",affected_rows=1)
        sql("SELECT id FROM t WHERE s='changed' ORDER BY id",rows=[])
        sql('SELECT s,v FROM t WHERE id=0',rows=[['reused',[3,4]]])
        peak = peak_bytes(client.p.pid)
        assert peak <= args.max_mb*1024*1024,peak
        client.kill(); client.start()
        sql('SELECT s,v FROM t WHERE id=0',rows=[['reused',[3,4]]])
        report.update(passed=True,mutation_rollback_reinsert_and_crash=True,peak_bytes=peak)
finally:
    if args.output:
        args.output.parent.mkdir(parents=True,exist_ok=True)
        args.output.write_text(json.dumps(report,ensure_ascii=False,indent=2)+'\n',encoding='utf-8')
    print(json.dumps(report,ensure_ascii=False),flush=True)
