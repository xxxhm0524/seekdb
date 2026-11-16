"""Paired native JSONL runs with an external scalar oracle and frozen inputs.

Run beside native_test_support.py in the dedicated Linux validation workspace.
Both database processes use the same read-only root, private /data, no network,
UID 65534 and enforced 2 CPU / 2 GiB / no-swap limits. This is not a score.
"""
import argparse
import hashlib
import json
import math
import os
import random
import statistics
import struct
import subprocess
import time
from pathlib import Path

from native_test_support import NativeSession, ROOT, save


def digest(path): return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def workload(out,n,q,with_body,ordered_keys=False):
    rng=random.Random(2026111507)
    rows=[[rng.randrange(-2048,2049)/128 for _ in range(128)] for _ in range(n)]
    queries=[[rng.randrange(-2048,2049)/128 for _ in range(128)] for _ in range(q)]
    order=list(range(n))
    if not ordered_keys:rng.shuffle(order)
    literal=lambda v:json.dumps(v,separators=(',',':'))
    body=lambda k:'record'+str(k)+':'+chr(97+k%26)*256
    inserts=['INSERT INTO t VALUES'+','.join(f"({k},{k%10},'g{k%10:02d}','{literal(rows[k])}','{body(k)}')" for k in order[i:i+100])+';' for i in range(0,n,100)]
    data=out/'oracle-input.bin'
    with data.open('wb') as f:
        f.write(struct.pack('<III',n,q,128))
        for v in rows+queries:f.write(struct.pack('<128f',*v))
    with (out/'oracle.txt').open('wb') as f:subprocess.run([str(ROOT/'build/benchmark-oracle'),str(data)],stdout=f,check=True)
    truth=[[(int(x.split(':')[0]),float(x.split(':')[1])) for x in line.split()] for line in (out/'oracle.txt').read_text().splitlines()]
    assert len(truth)==q
    sql={}
    for kind in ('bigint','text'):
        sql[kind]=[]
        for i,v in enumerate(queries):
            where='' if not i%2 else (' WHERE g='+str((i//2)%10) if kind=='bigint' else " WHERE category='g%02d'"%((i//2)%10))
            sql[kind].append(f"SELECT id,L2_DISTANCE(v,'{literal(v)}') AS d"+(",body" if with_body else "")+f" FROM t{where} ORDER BY d LIMIT 10;")
    fixtures_sha=hashlib.sha256(('\n'.join(inserts+sql['bigint']+sql['text'])).encode()).hexdigest()
    return inserts,sql,truth,body,fixtures_sha


def resources(db):
    p=Path('/proc')/str(db.proc.pid)
    status={k:v.strip() for k,v in (line.split(':',1) for line in (p/'status').read_text().splitlines() if ':' in line)}
    io={k:int(v) for k,v in (line.split(':',1) for line in (p/'io').read_text().splitlines())}
    return dict(process_vmhwm_kib=int(status.get('VmHWM','0 kB').split()[0]),process_io=io,
                cgroup=(p/'cgroup').read_text(),uid=status['Uid'])


def run(args):
    out=Path(args.output);out.mkdir(exist_ok=False)
    candidates={'baseline':Path(args.baseline),'candidate':Path(args.candidate)}
    inserts,sql,truth,body,fixtures_sha=workload(out,args.rows,args.queries,args.with_body)
    samples=[]
    def verify(response,index):
        assert response['ok'] is True and response['columns']==(['id','d','body'] if args.with_body else ['id','d'])
        assert len(response['rows'])==len(truth[index])
        for row,(k,d) in zip(response['rows'],truth[index]):
            assert row[0]==k and (not args.with_body or row[2]==body(k)) and math.isclose(row[1],d,rel_tol=1e-12,abs_tol=1e-12),(index,row,k,d)
    for round_no in range(args.rounds):
        shapes=[(kind,indexed) for kind in ('bigint','text') for indexed in (False,True)]
        if round_no%2:shapes.reverse()
        for shape_no,(kind,indexed) in enumerate(shapes):
            versions=['baseline','candidate']
            if (round_no+shape_no)%2:versions.reverse()
            for name in versions:
                tag=f'r{round_no+1}-{kind}-idx{int(indexed)}-{name}'
                db=NativeSession(out,tag,candidates[name]);rec=dict(round=round_no+1,kind=kind,indexed=indexed,version=name)
                try:
                    def request(statement):
                        start=time.perf_counter_ns();response=db.execute(statement)
                        return response,(time.perf_counter_ns()-start)/1e6
                    _,rec['create_ms']=request('CREATE TABLE t(id BIGINT PRIMARY KEY,g BIGINT,category VARCHAR(4),v VECTOR(128),body VARCHAR(512));')
                    writes=[]
                    for statement in inserts:
                        response,elapsed=request(statement);assert response['affected_rows']==100;writes.append(elapsed)
                    rec['write_ms']=sum(writes);rec['write_requests_ms']=writes
                    rec['index_ms']=0.0
                    if indexed:_,rec['index_ms']=request('CREATE VECTOR INDEX iv ON t(v);')
                    for index in range(10):verify(db.execute(sql[kind][index%args.queries]),index%args.queries)
                    rec['resources_before_queries']=resources(db)
                    timings=[]
                    for index,statement in enumerate(sql[kind]):
                        response,elapsed=request(statement);timings.append(elapsed);verify(response,index)
                    rec.update(query_ms=sum(timings),unfiltered_ms=sum(timings[::2]),filtered_ms=sum(timings[1::2]),query_requests_ms=timings,resources=resources(db))
                    rec['cycle_ms']=rec['write_ms']+rec['index_ms']+rec['query_ms']
                    rec['query_io_delta']={k:rec['resources']['process_io'][k]-rec['resources_before_queries']['process_io'][k] for k in rec['resources']['process_io']}
                    for index in range(4):verify(db.execute(sql[kind][index],mode='ann'),index)
                    db.restart(crash=True);verify(db.execute(sql[kind][0]),0)
                    rec.update(passed=True,queries_verified=args.queries,restart_verified=True)
                    samples.append(rec);save(out/(tag+'.json'),rec)
                    print(json.dumps({k:rec[k] for k in ('round','kind','indexed','version','write_ms','query_ms','cycle_ms','passed')}),flush=True)
                finally:
                    try:db.shutdown()
                    finally:db.close()
    summaries={}
    for kind in ('bigint','text'):
        for indexed in (False,True):
            group={name:[r for r in samples if r['version']==name and r['kind']==kind and r['indexed']==indexed] for name in candidates}
            medians={name:{k:statistics.median(r[k] for r in values) for k in ('write_ms','query_ms','cycle_ms')} for name,values in group.items()}
            paired={k:[group['baseline'][i][k]/group['candidate'][i][k] for i in range(args.rounds)] for k in ('write_ms','query_ms','cycle_ms')}
            summaries[f'{kind}-idx{int(indexed)}']=dict(medians=medians,paired_speedups=paired,median_paired_speedup={k:statistics.median(v) for k,v in paired.items()})
    report=dict(passed=True,with_body=args.with_body,rows=args.rows,dimensions=128,queries=args.queries,rounds=args.rounds,batch=100,warmup=10,
                cpu_affinity=sorted(os.sched_getaffinity(0)),
                versions={name:dict(binary=str(path),sha256=digest(path)) for name,path in candidates.items()},
                fixture_sha256=fixtures_sha,oracle_source_sha256=digest(ROOT/'benchmark-oracle.cpp'),oracle_binary_sha256=digest(ROOT/'build/benchmark-oracle'),
                samples=len(samples),verified_queries=len(samples)*args.queries,summary=summaries,
                timing='Sum of actual JSONL request round trips; input and oracle generation, warmup, verification and restart excluded. Fresh database per sample; version/order counterbalanced.',
                resources='Per-process VmHWM and /proc/io retained; not formal T/B/M. CPU quota and memory ceiling enforced by launcher.',formal_score=None)
    save(out/'report.json',report)
    print(json.dumps(report),flush=True)


if __name__=='__main__':
    parser=argparse.ArgumentParser();parser.add_argument('--output',required=True)
    parser.add_argument('--baseline',required=True);parser.add_argument('--candidate',required=True)
    parser.add_argument('--with-body',action='store_true');parser.add_argument('--rounds',type=int,default=3);parser.add_argument('--rows',type=int,default=10000);parser.add_argument('--queries',type=int,default=1000)
    parser.add_argument('--affinity-cpus',help='Comma-separated existing CPU IDs; binds only this runner and its child processes')
    args=parser.parse_args();assert args.rows%100==0 and args.queries>=10 and args.rounds>=1
    if args.affinity_cpus:os.sched_setaffinity(0,{int(n) for n in args.affinity_cpus.split(',')})
    run(args)
