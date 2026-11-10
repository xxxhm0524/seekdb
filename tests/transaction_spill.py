#!/usr/bin/env python3
"""Long JSONL transactions exceed the payload budget, preserve savepoints,
commit/recover, roll back, and clean up abandoned spill files."""
import argparse
from contextlib import ExitStack
from pathlib import Path
import sys
import tempfile

sys.path.insert(0,str(Path(__file__).resolve().parent))
from protocol_client import Client
from resource_boundaries import peak_bytes


def main():
    parser=argparse.ArgumentParser()
    parser.add_argument('--rows',type=int,default=12000)
    parser.add_argument('--max-mb',type=int,default=80)
    args=parser.parse_args()
    with tempfile.TemporaryDirectory(prefix='mvd-spill-') as data, ExitStack() as cleanup:
        client=Client(datadir=data,extra=['--cache-mb=1','--tx-memory-mb=1'])
        cleanup.callback(client.close)
        peak_seen=0
        def sql(text,**fields):
            r=client.request(text)
            assert r.get('ok') is True,(text[:120],r)
            for key,value in fields.items():assert r.get(key)==value,(text[:120],r)
            return r
        def batch(start,stop):
            return ','.join("(%d,'x','[%d,0]')"%(i,i%10) for i in range(start,stop))
        sql('CREATE TABLE t(id BIGINT PRIMARY KEY,s VARCHAR(2048),v VECTOR(2))')
        sql('BEGIN')
        sql('INSERT INTO t VALUES '+batch(1,101),affected_rows=100)
        # Failure crosses the spill threshold for the first time.
        r=client.request('INSERT INTO t VALUES '+batch(101,251)+",(1,'dup','[0,0]')")
        assert r.get('ok') is False,r
        sql('SELECT id FROM t WHERE id=250',rows=[])
        sql('SELECT s FROM t WHERE id=1',rows=[['x']])
        sql('ROLLBACK')
        assert not (Path(data)/'txn.tmp').exists()
        for commit in (False,True):
            sql('BEGIN')
            for start in range(1,args.rows+1,100):
                stop=min(start+100,args.rows+1)
                sql('INSERT INTO t VALUES '+batch(start,stop),affected_rows=stop-start)
                if start%1000==1:
                    peak=peak_bytes(client.p.pid)
                    peak_seen=max(peak_seen,peak)
                    assert peak<=args.max_mb*2**20,('transaction memory',peak/2**20)
            assert (Path(data)/'txn.tmp').exists(),'payload never spilled'
            before=(Path(data)/'txn.tmp').stat().st_size
            r=client.request('INSERT INTO t VALUES '+batch(args.rows+1,args.rows+5)+",(1,'dup','[0,0]')")
            assert r.get('ok') is False,r
            assert (Path(data)/'txn.tmp').stat().st_size==before,'failed statement leaked spill space'
            sql("UPDATE t SET s='u',v='[0,0]' WHERE id=1",affected_rows=1)
            sql('SELECT id,s,v FROM t WHERE id=1',rows=[[1,'u',[0,0]]])
            sql('COMMIT' if commit else 'ROLLBACK')
            peak_seen=max(peak_seen,peak_bytes(client.p.pid))
            assert peak_seen<=args.max_mb*2**20,('commit memory',peak_seen/2**20)
            assert not (Path(data)/'txn.tmp').exists(),'transaction spool not released'
            if not commit:sql('SELECT id FROM t',rows=[])
        client.kill();client.start()
        ids=sql('SELECT id FROM t ORDER BY id')['rows']
        assert ids==[[i] for i in range(1,args.rows+1)]
        sql('SELECT s FROM t WHERE id=1',rows=[['u']])
        sql('BEGIN')
        sql('INSERT INTO t VALUES '+batch(args.rows+1,args.rows+401),affected_rows=400)
        assert (Path(data)/'txn.tmp').exists()
        sql("UPDATE t SET s='dirty' WHERE id=1",affected_rows=1)
        client.kill();client.start()
        assert not (Path(data)/'txn.tmp').exists(),'abandoned spool survived restart'
        sql('SELECT id FROM t WHERE id=%d'%(args.rows+1),rows=[])
        sql('SELECT s FROM t WHERE id=1',rows=[['u']])
        # Overwriting the same column must not retain every historical value.
        sql('BEGIN')
        for turn in range(600):
            text=('repeat%d'%turn).ljust(2048,'z')
            sql("UPDATE t SET s='%s' WHERE id=1"%text,affected_rows=1)
        assert not (Path(data)/'txn.tmp').exists(),'shadowed patch history exceeded budget'
        sql('ROLLBACK')
        # Distinct UPDATE values (not just INSERT rows) also spill by budget.
        changed=min(2000,args.rows-1)
        for commit in (False,True):
            sql('BEGIN')
            for ident in range(2,changed+2):
                text=('value%d'%ident).ljust(2048,'z')
                sql("UPDATE t SET s='%s' WHERE id=%d"%(text,ident),affected_rows=1)
            assert (Path(data)/'txn.tmp').exists(),'UPDATE payload never spilled'
            sql('COMMIT' if commit else 'ROLLBACK')
            assert not (Path(data)/'txn.tmp').exists()
            if not commit:sql('SELECT s FROM t WHERE id=2',rows=[['x']])
        client.kill();client.start()
        for start in range(2,changed+2,200):
            stop=min(start+200,changed+2)
            expected=[[ident,('value%d'%ident).ljust(2048,'z')] for ident in range(start,stop)]
            sql('SELECT id,s FROM t WHERE id>=%d AND id<%d ORDER BY id'%(start,stop),rows=expected)
        sql("UPDATE t SET s='x' WHERE id>=2 AND id<=%d"%(changed+1),affected_rows=changed)
        sql("DELETE FROM t WHERE s='x'",affected_rows=args.rows-1)
        sql('SELECT id FROM t',rows=[[1]])
        print('transaction spill: PASS (%d wide rows, peak %.2f MiB, commit/rollback/failure/crash)'%
            (args.rows,peak_seen/2**20))


if __name__=='__main__':main()
