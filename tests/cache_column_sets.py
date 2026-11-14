"""Mixed scalar filters, uncached projections and cache refusal share one root."""
import argparse
import json
import math
from pathlib import Path
import sys
from client import Client

def run(directory):
    db=Client(directory,timeout=60,env={'MVD_VECTOR_CACHE_MIB':'2'})
    count,dimensions=2500,33
    payload='x'*2048
    rows={k:(f'c{k%5}',k%7,[(k%23)/8.0]*dimensions,payload) for k in range(count)}
    checked=0
    try:
        db.execute(f'CREATE TABLE t(v VECTOR({dimensions}),id BIGINT PRIMARY KEY,category VARCHAR(12),group_id BIGINT,content VARCHAR(2048));')
        for base in range(0,count,100):
            values=','.join(f"('{json.dumps(rows[k][2])}',{k},'{rows[k][0]}',{rows[k][1]},'{payload}')" for k in range(base,base+100))
            db.execute('INSERT INTO t VALUES'+values+';')
        db.execute('CREATE VECTOR INDEX idx ON t(v);')
        def query(where,match,body=False):
            nonlocal checked
            ids=[k for k,r in rows.items() if match(k,r)]
            distance={k:math.sqrt(sum(float(x)*float(x) for x in rows[k][2])) for k in ids}
            ids.sort(key=lambda k:(distance[k],k));ids=ids[:11]
            projection='id,content' if body else 'id'
            statement=f"SELECT {projection},L2_DISTANCE(v,'[{','.join('0' for _ in range(dimensions))}]') AS d FROM t {where} ORDER BY d LIMIT 11;"
            actual=db.execute(statement)['rows'];assert [r[0] for r in actual]==ids
            for record,k in zip(actual,ids):
                assert math.isclose(record[-1],distance[k],rel_tol=1e-12,abs_tol=1e-12)
                if body:assert record[1]==rows[k][3]
            checked+=1
        for iteration in range(45):
            if iteration%3==0:query("WHERE category='c3'",lambda k,r:r[0]=='c3',iteration%9==0)
            elif iteration%3==1:query('WHERE group_id=2',lambda k,r:r[1]==2)
            else:query("WHERE category='c1' AND group_id=6",lambda k,r:r[0]=='c1' and r[1]==6)
        query("WHERE content='"+payload+"'",lambda k,r:r[3]==payload)
        query("WHERE category='c3'",lambda k,r:r[0]=='c3',True)
        db.execute('BEGIN;');db.execute("UPDATE t SET category='c3',group_id=2,v='["+','.join('0' for _ in range(dimensions))+" ]',content='pending' WHERE id=1;")
        original=rows[1];rows[1]=('c3',2,[0.0]*dimensions,'pending')
        query("WHERE category='c3' AND group_id=2",lambda k,r:r[0]=='c3' and r[1]==2,True)
        db.execute('ROLLBACK;');rows[1]=original
        query("WHERE category='c3' AND group_id=2",lambda k,r:r[0]=='c3' and r[1]==2,True)
        db.restart(crash=True)
        query('WHERE group_id=2',lambda k,r:r[1]==2,True)
        return dict(passed=True,rows=count,dimensions=dimensions,cache_mib=2,queries_checked=checked,
                    mixed_filter_columns=True,large_filter_refusal=True,winner_projection=True,rollback_and_restart=True)
    finally:db.kill()

if __name__=='__main__':
    sys.stdout.reconfigure(encoding='utf-8')
    p=argparse.ArgumentParser();p.add_argument('--output',required=True);args=p.parse_args()
    out=Path(args.output);out.mkdir(parents=True,exist_ok=False)
    try:result=run(out/'data')
    except Exception as error:result=dict(passed=False,error=str(error))
    (out/'report.json').write_text(json.dumps(result,indent=2),encoding='utf-8');print(json.dumps(result),flush=True)
    sys.exit(0 if result['passed'] else 1)
