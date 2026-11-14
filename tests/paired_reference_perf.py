"""Paired defaults-vs-successful-code comparison under the same toolchain.

This measures driver-observed time and container memory. Raw io.stat is retained
without pretending that it is official isolated-device B or a competition score.
"""
import argparse
import hashlib
import json
import math
from pathlib import Path
import statistics
import subprocess
import sys
import time
import numpy as np
from client import Client, ROOT
from reference_differential import fetch_reference

def corpus(seed,body_width,rows=10000,dimensions=128,queries=1000):
    rng=np.random.default_rng(seed)
    vectors=rng.uniform(-1,1,(rows,dimensions)).astype(np.float32)
    vectors[1]=vectors[0]
    groups=rng.integers(0,7,rows)
    alphabet=np.array(list('abcdefghijklmnopqrstuvwxyz0123456789'))
    bodies=[''.join(rng.choice(alphabet,body_width)) for _ in range(rows)]
    inserts=[]
    for base in range(0,rows,100):
        values=[f"({k},'c{groups[k]}','{bodies[k]}','{json.dumps(vectors[k].tolist(),separators=(',',':'))}')" for k in range(base,min(base+100,rows))]
        inserts.append('INSERT INTO docs VALUES'+','.join(values)+';')
    query_vectors=rng.uniform(-1,1,(queries,dimensions)).astype(np.float32)
    filters=[None if i%2==0 else int(rng.integers(0,7)) for i in range(queries)]
    statements=[]
    for i,q in enumerate(query_vectors):
        where='' if filters[i] is None else f"WHERE category='c{filters[i]}'"
        statements.append(f"SELECT id,L2_DISTANCE(embedding,'{json.dumps(q.tolist(),separators=(',',':'))}') AS distance FROM docs {where} ORDER BY distance LIMIT 10;")
    # Compute truth before either timed run, with dimension-ordered float64 sums.
    values=vectors.astype(np.float64);truth=[]
    for q,group in zip(query_vectors,filters):
        sums=np.zeros(rows,dtype=np.float64)
        for d in range(dimensions):
            delta=values[:,d]-float(q[d]);sums+=delta*delta
        distances=np.sqrt(sums)
        keys=np.arange(rows) if group is None else np.flatnonzero(groups==group)
        ordered=sorted(keys.tolist(),key=lambda k:(float(distances[k]),k))[:10]
        truth.append([[k,float(distances[k])] for k in ordered])
    return dict(seed=seed,width=body_width,rows=rows,dimensions=dimensions,inserts=inserts,
                queries=statements,truth=truth,vectors=vectors,groups=groups,bodies=bodies)

def measure(image,directory,data):
    db=Client(directory,timeout=60,env={'MVD_DOCKER_IMAGE':image,'MVD_DOCKER_MEMORY':'2g'})
    try:
        db.execute(f"CREATE TABLE docs(id BIGINT PRIMARY KEY,category VARCHAR(16),content VARCHAR(1024),embedding VECTOR({data['dimensions']}));")
        start=time.perf_counter()
        for sql in data['inserts']:
            db.execute('BEGIN;');db.execute(sql);db.execute('COMMIT;')
        write=time.perf_counter()-start
        start=time.perf_counter();db.execute('CREATE VECTOR INDEX v_idx ON docs(embedding);')
        for sql in data['queries'][:10]:db.execute(sql)
        prepare=time.perf_counter()-start
        start=time.perf_counter();answers=[db.execute(sql)['rows'] for sql in data['queries']];query=time.perf_counter()-start
        peak=int(subprocess.check_output(['docker','exec',db.container,'cat','/sys/fs/cgroup/memory.peak']))
        io=subprocess.check_output(['docker','exec',db.container,'cat','/sys/fs/cgroup/io.stat'],text=True)
        for actual,expected in zip(answers,data['truth']):
            assert [r[0] for r in actual]==[r[0] for r in expected]
            for (_,a),(_,b) in zip(actual,expected):assert math.isclose(a,b,rel_tol=1e-5,abs_tol=1e-5)
        files=sum(p.stat().st_size for p in Path(directory).rglob('*') if p.is_file())
        db.restart(crash=True)
        assert db.execute('SELECT id FROM docs ORDER BY id;')['rows']==[[k] for k in range(data['rows'])]
        for k in [0,1,59,data['rows']//2,data['rows']-1]:
            row=db.execute(f'SELECT content,category,embedding FROM docs WHERE id={k};')['rows'][0]
            assert row[0]==data['bodies'][k] and row[1]==f"c{data['groups'][k]}"
            assert np.allclose(row[2],data['vectors'][k].astype(np.float64),rtol=1e-5,atol=1e-5)
        return dict(passed=True,write_seconds=write,prepare_seconds=prepare,query_seconds=query,
                    memory_peak_bytes=peak,raw_io_stat=io,storage_bytes=files,exact_ids_checked=len(answers))
    finally:db.kill()

def run(output,ours,rounds):
    source,manifest=fetch_reference(output)
    reference='mvd-public-controlled:'+manifest['commit'][:12]
    with (output/'reference-build.log').open('wb') as log:
        subprocess.run(['docker','build','--platform','linux/amd64','-f',str(ROOT/'tests/reference-controlled.Dockerfile'),'-t',reference,str(source)],check=True,stdout=log,stderr=subprocess.STDOUT)
    records=[]
    for width in [16,1024]:
        for round in range(rounds):
            data=corpus(91271+round,width)
            order=[('ours',ours),('reference',reference)] if round%2==0 else [('reference',reference),('ours',ours)]
            result=dict(body_bytes=width,round=round,seed=data['seed'])
            for name,image in order:
                result[name]=measure(image,output/f'body{width}-round{round}-{name}',data)
            records.append(result);print(json.dumps(result),flush=True)
            (output/'rounds.json').write_text(json.dumps(records,indent=2),encoding='utf-8')
    medians={}
    for width in [16,1024]:
        selected=[r for r in records if r['body_bytes']==width]
        summary={name:{key:statistics.median(r[name][key] for r in selected) for key in ['write_seconds','query_seconds','memory_peak_bytes','storage_bytes']} for name in ['ours','reference']}
        summary['ours_over_reference_query_time']=summary['ours']['query_seconds']/summary['reference']['query_seconds']
        medians[str(width)]=summary
    return dict(passed=True,rounds=records,medians=medians,reference_commit=manifest['commit'],reference_evidence=manifest['formal_result_evidence'],
                environment='Each server: same GCC 13.5 O3 toolchain and Ubuntu runtime; 2 CPU, 2 GiB, swap disabled, read-only root and no network. This controls the toolchain and is not the reference author\'s original Alpine/GCC 14 production image.',
                scope='Input SQL and independent truth are prepared outside timers. Timers include request serialization, IPC, responses and server persistence. Query prep and oracle work are excluded. Memory peak includes prep and page cache. Raw io.stat is observational; no official B, E, rank or score is inferred.')

if __name__=='__main__':
    sys.stdout.reconfigure(encoding='utf-8')
    p=argparse.ArgumentParser();p.add_argument('--output',required=True);p.add_argument('--image',required=True);p.add_argument('--rounds',type=int,default=3);args=p.parse_args()
    out=Path(args.output).resolve();out.mkdir(parents=True,exist_ok=False)
    try:result=run(out,args.image,args.rounds)
    except Exception as error:
        import traceback
        result=dict(passed=False,error=str(error),traceback=traceback.format_exc())
    (out/'report.json').write_text(json.dumps(result,ensure_ascii=False,indent=2),encoding='utf-8')
    print(json.dumps({k:v for k,v in result.items() if k!='rounds'},ensure_ascii=False),flush=True)
    sys.exit(0 if result['passed'] else 1)
