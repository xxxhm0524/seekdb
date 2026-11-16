"""Interleaved paired queries, with driver and server on distinct fixed CPUs."""
import argparse
import hashlib
import json
import math
import os
from pathlib import Path
import statistics
import time
from native_test_support import NativeSession,ROOT,save
from native_ab_e860 import workload,resources


def run(a):
    out=Path(a.output);out.mkdir(exist_ok=False)
    os.sched_setaffinity(0,{a.driver_cpu});os.environ['MVD_NATIVE_CPU']=str(a.server_cpu)
    inserts,queries,truth,body,fixture_hash=workload(out,a.rows,a.queries,a.with_body)
    programs={'baseline':Path(a.baseline),'candidate':Path(a.candidate)};records=[]
    def check(r,i):
        assert r['ok'] and r['columns']==(['id','d','body'] if a.with_body else ['id','d'])
        assert len(r['rows'])==len(truth[i])
        for x,(key,d) in zip(r['rows'],truth[i]):assert x[0]==key and (not a.with_body or x[2]==body(key)) and math.isclose(x[1],d,abs_tol=1e-12,rel_tol=1e-12)
    for kind in ([a.kind] if a.kind!='all' else ['bigint','text']):
        for indexed in ([a.indexed=='yes'] if a.indexed!='all' else [False,True]):
            dbs={}
            try:
                for name,path in programs.items():
                    db=NativeSession(out,kind+'-'+str(indexed)+'-'+name,path);dbs[name]=db
                    assert os.sched_getaffinity(db.proc.pid)=={a.server_cpu}
                    db.execute('CREATE TABLE t(id BIGINT PRIMARY KEY,g BIGINT,category VARCHAR(4),v VECTOR(128),body VARCHAR(512));')
                    for sql in inserts:assert db.execute(sql)['affected_rows']==100
                    if indexed:db.execute('CREATE VECTOR INDEX iv ON t(v);')
                    for i in range(10):check(db.execute(queries[kind][i]),i)
                for round_no in range(a.rounds):
                    before={name:resources(db) for name,db in dbs.items()};elapsed={name:0 for name in dbs}
                    block_size=a.queries if a.whole else 50
                    for first in range(0,a.queries,block_size):
                        names=list(dbs)
                        if (round_no+first//block_size)%2:names.reverse()
                        for name in names:
                            db=dbs[name]
                            for i in range(first,min(first+block_size,a.queries)):
                                start=time.perf_counter_ns();r=db.execute(queries[kind][i]);elapsed[name]+=time.perf_counter_ns()-start;check(r,i)
                    for name,db in dbs.items():
                        after=resources(db)
                        records.append(dict(kind=kind,indexed=indexed,round=round_no+1,version=name,query_ms=elapsed[name]/1e6,
                            query_io_delta={k:after['process_io'][k]-before[name]['process_io'][k] for k in after['process_io']},resources=after))
                    print(json.dumps(dict(kind=kind,indexed=indexed,round=round_no+1,query_ms={n:x/1e6 for n,x in elapsed.items()},speedup=elapsed['baseline']/elapsed['candidate'])),flush=True)
                for db in dbs.values():db.restart(crash=True);check(db.execute(queries[kind][0]),0)
            finally:
                for db in dbs.values():
                    try:db.shutdown()
                    finally:db.close()
    summary={}
    for kind in ([a.kind] if a.kind!='all' else ['bigint','text']):
        for indexed in ([a.indexed=='yes'] if a.indexed!='all' else [False,True]):
            groups={n:[r for r in records if r['kind']==kind and r['indexed']==indexed and r['version']==n] for n in programs}
            ratios=[b['query_ms']/c['query_ms'] for b,c in zip(groups['baseline'],groups['candidate'])]
            summary[kind+'-idx'+str(int(indexed))]=dict(paired_speedups=ratios,median_paired_speedup=statistics.median(ratios),
                median_ms={n:statistics.median(r['query_ms'] for r in v) for n,v in groups.items()},
                median_read_calls={n:statistics.median(r['query_io_delta']['syscr'] for r in v) for n,v in groups.items()})
    report=dict(passed=True,rows=a.rows,queries=a.queries,rounds=a.rounds,with_body=a.with_body,driver_cpu=a.driver_cpu,server_cpu=a.server_cpu,
        versions={n:hashlib.sha256(p.read_bytes()).hexdigest() for n,p in programs.items()},fixture_sha256=fixture_hash,summary=summary,records=records,
        verified_queries=len(summary)*a.rounds*a.queries*2,scope=('Real JSONL request times, whole-version rounds.' if a.whole else 'Real JSONL request times, alternating blocks of 50.')+' Fixed server CPU is not reserved; driver on a distinct CPU. No official score inferred.')
    save(out/'report.json',report);print(json.dumps({k:v for k,v in report.items() if k!='records'}),flush=True)


if __name__=='__main__':
    p=argparse.ArgumentParser();p.add_argument('--output',required=True);p.add_argument('--baseline',required=True);p.add_argument('--candidate',required=True)
    p.add_argument('--rows',type=int,default=10000);p.add_argument('--queries',type=int,default=1000);p.add_argument('--rounds',type=int,default=6);p.add_argument('--with-body',action='store_true');p.add_argument('--driver-cpu',type=int,default=3);p.add_argument('--server-cpu',type=int,default=2)
    p.add_argument('--kind',choices=['all','bigint','text'],default='bigint')
    p.add_argument('--indexed',choices=['all','no','yes'],default='no')
    p.add_argument('--whole',action='store_true')
    p.add_argument('--gate',action='store_true')
    p.add_argument('--gate-slack',type=float,default=0.01)
    a=p.parse_args();run(a)
    report=json.loads((Path(a.output)/'report.json').read_text())
    changes={k:100*(1/v['median_paired_speedup']-1) for k,v in report['summary'].items()}
    gate=dict(performance_gate_passed=max(changes.values())<=a.gate_slack*100,regression_percent=changes,allowed_regression_percent=a.gate_slack*100,whole=a.whole)
    save(Path(a.output)/'performance-gate.json',gate);print(json.dumps(gate),flush=True)
    if a.gate:raise SystemExit(0 if gate['performance_gate_passed'] else 2)
