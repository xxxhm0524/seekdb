"""Measure the actual process peak for large valid query-vector binding."""
import argparse
import hashlib
import json
import os
import statistics
import time
from pathlib import Path
from native_test_support import NativeSession, ROOT, save


def run(args):
    out=Path(args.output);out.mkdir(exist_ok=False)
    versions={'baseline':Path(args.baseline),'candidate':Path(args.candidate)}
    samples=[]
    for dims in (128,131072,1048576,4194304):
        vector='['+','.join(['0']*dims)+']'
        sql=f"SELECT L2_DISTANCE(v,'{vector}') AS d FROM t LIMIT 0;"
        wire_size=len((json.dumps(dict(id=2,op='execute',sql=sql,search_mode='exact'))+'\n').encode())
        assert wire_size<=16777216
        for round_no in range(4):
            names=list(versions)
            if round_no%2:names.reverse()
            for name in names:
                db=NativeSession(out,f'd{dims}-r{round_no}-{name}',versions[name])
                try:
                    db.execute(f'CREATE TABLE t(id BIGINT PRIMARY KEY,v VECTOR({dims}));')
                    start=time.perf_counter_ns();value=db.execute(sql);elapsed=(time.perf_counter_ns()-start)/1e6
                    assert value['columns']==['d'] and value['rows']==[]
                    status=dict(line.split(':',1) for line in (Path('/proc')/str(db.proc.pid)/'status').read_text().splitlines() if ':' in line)
                    peak=int(status['VmHWM'].split()[0])
                    samples.append(dict(dimension=dims,round=round_no,version=name,request_bytes=wire_size,elapsed_ms=elapsed,process_vmhwm_kib=peak,passed=True))
                finally:
                    try:db.shutdown()
                    finally:db.close()
    summary={}
    for dims in (128,131072,1048576,4194304):
        summary[str(dims)]={name:{key:statistics.median(r[key] for r in samples if r['dimension']==dims and r['version']==name) for key in ('elapsed_ms','process_vmhwm_kib')} for name in versions}
    report=dict(passed=True,samples=samples,summary=summary,versions={n:hashlib.sha256(p.read_bytes()).hexdigest() for n,p in versions.items()},cpu_affinity=sorted(os.sched_getaffinity(0)),scope='Actual default JSONL process; valid zero-result distance projection still binds and validates its full vector. Per-process VmHWM includes startup and parsing. This is a wide-vector micro workload, not formal memory or a 128D workload speed claim.')
    save(out/'report.json',report);print(json.dumps(dict(passed=True,summary=summary)),flush=True)


if __name__=='__main__':
    p=argparse.ArgumentParser();p.add_argument('--output',required=True);p.add_argument('--baseline',required=True);p.add_argument('--candidate',required=True)
    args=p.parse_args();os.sched_setaffinity(0,{2,3});run(args)
