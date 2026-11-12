#!/usr/bin/env python3
"""Sort >128 MiB of effective string keys in a 128 MiB final runtime."""
import argparse
import json
from pathlib import Path
import sys
import time

ROOT=Path(__file__).resolve().parent.parent
sys.path.insert(0,str(ROOT/'protocol_check'))
from _transport import Session

parser=argparse.ArgumentParser()
parser.add_argument('--image',required=True)
parser.add_argument('--rows',type=int,default=20000)
parser.add_argument('--output',type=Path,default=ROOT/'protocol-results/docker-ordered-keys.json')
args=parser.parse_args()
args.output.parent.mkdir(parents=True,exist_ok=True)
config=json.loads((ROOT/'protocol_check/config.json').read_text(encoding='utf-8'))
config['memory']='128m'
session=Session(config,args.output.with_suffix('.stderr.log'),image=args.image)
texts=[p+'\U0001f600'*2047 for p in ['b','a','中','\U0001f642']]
effective=sum(len(texts[i%4].encode('utf-8')) for i in range(args.rows))
assert effective>128*1024*1024
report=dict(passed=False,rows=args.rows,effective_key_bytes=effective,memory_limit_bytes=128*1024*1024,stages=[])
report['scope']='additional functional/resource stress; diagnostic timings include client assertions; not official scoring'
def sql(text,**fields):
    value=session.request(text)
    assert value.get('ok') is True,(text[:100],value)
    for key,wanted in fields.items():assert value.get(key)==wanted,(key,value)
    return value
def snapshot(stage,seconds):
    raw=session.docker('exec',session.name,'cat','/sys/fs/cgroup/memory.current',
        '/sys/fs/cgroup/memory.peak','/proc/1/status').stdout.decode().splitlines()
    row=dict(stage=stage,seconds=round(seconds,3),memory_current_bytes=int(raw[0]),memory_peak_bytes=int(raw[1]),
        process_memory=[line for line in raw[2:] if line.startswith(('VmHWM:','VmRSS:'))])
    report['stages'].append(row);print(json.dumps(row),flush=True)
try:
    report['active_stage']='startup'
    session.start()
    report['active_stage']='create table'
    sql('CREATE TABLE t(id BIGINT PRIMARY KEY,g BIGINT,s VARCHAR(2048),v VECTOR(2))')
    for first in range(0,args.rows,100):
        report['active_stage']='load batch %d'%first
        last=min(first+100,args.rows)
        sql('INSERT INTO t VALUES '+','.join("(%d,%d,'%s','[%d,0]')"%(i,i%7,texts[i%4],i)
            for i in range(first,last)),affected_rows=last-first)
    report['active_stage']='restart after load'
    session.shutdown();session.start()
    snapshot('startup',0)
    oracle=sorted(range(args.rows),key=lambda i:(texts[i%4],-(i%7),i))
    before=time.perf_counter()
    report['active_stage']='wide-key one-row OFFSET'
    sql('SELECT id FROM t ORDER BY s ASC,g DESC,id ASC LIMIT 1 OFFSET %d'%(args.rows-1),rows=[[oracle[-1]]])
    snapshot('wide-key one-row OFFSET',time.perf_counter()-before)
    before=time.perf_counter()
    report['active_stage']='wide-key full ID projection'
    sql('SELECT id FROM t ORDER BY s ASC,g DESC,id ASC',rows=[[i] for i in oracle])
    snapshot('wide-key full ID projection',time.perf_counter()-before)
    descending=sorted(range(args.rows),key=lambda i:(texts[i%4],-i),reverse=True)
    first=args.rows//2
    before=time.perf_counter()
    report['active_stage']='wide-key mixed distance order'
    sql("SELECT id,L2_DISTANCE(v,'[0,0]') AS d FROM t ORDER BY s DESC,d ASC,id ASC LIMIT 3 OFFSET %d"%first,
        rows=[[i,i] for i in descending[first:first+3]])
    snapshot('wide-key mixed distance order',time.perf_counter()-before)
    assert session.docker('exec',session.name,'test','-e','/data/sort.tmp',check=False).returncode!=0
    report['active_stage']='kill and recover'
    session.crash();session.start()
    sql('SELECT g,s FROM t WHERE id=0',rows=[[0,texts[0]]])
    session.shutdown()
    report.update(passed=True,ordered_ids_verified=True,mixed_distance_order_verified=True,
        scratch_removed=True,committed_data_recovered=True)
    report['active_stage']='finished'
except Exception as error:
    report['error']=str(error)
    state=session.docker('inspect','--format','{{json .State}}',session.name,check=False)
    report['docker_state']=state.stdout.decode(errors='replace')
    raise
finally:
    session.close()
    args.output.write_text(json.dumps(report,ensure_ascii=False,indent=2)+'\n',encoding='utf-8')
    print(json.dumps(report,ensure_ascii=False),flush=True)
