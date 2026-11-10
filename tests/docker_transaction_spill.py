#!/usr/bin/env python3
"""Commit >2 GiB of uncommitted rows inside the actual 256 MiB container."""
import argparse,json
from pathlib import Path
import sys,time
ROOT=Path(__file__).resolve().parent.parent
sys.path.insert(0,str(ROOT/'protocol_check'))
from _transport import Session

def main():
    parser=argparse.ArgumentParser()
    parser.add_argument('--image',required=True)
    parser.add_argument('--rows',type=int,default=270000)
    args=parser.parse_args()
    output=ROOT/'protocol-results/docker-transaction-spill.json'
    output.parent.mkdir(parents=True,exist_ok=True)
    config=json.loads((ROOT/'protocol_check/config.json').read_text(encoding='utf-8'))
    config['memory']='256m'
    session=Session(config,output.with_suffix('.stderr.log'),image=args.image)
    stages=[]
    def sql(text,**fields):
        r=session.request(text)
        assert r.get('ok') is True,(text[:160],r)
        for k,v in fields.items():assert r.get(k)==v,(text[:160],r)
        return r
    def exists():
        return session.docker('exec',session.name,'test','-e','/data/txn.tmp',check=False).returncode==0
    def size():
        return int(session.docker('exec',session.name,'stat','-c','%s','/data/txn.tmp').stdout)
    def record(stage,**values):
        peak=int(session.docker('exec',session.name,'cat','/sys/fs/cgroup/memory.peak').stdout)
        row=dict(stage=stage,memory_peak_bytes=peak,**values);stages.append(row)
        print(json.dumps(row),flush=True)
    def batch(start,stop):
        return ','.join("(%d,'x','[%d,0]')"%(i,i%10) for i in range(start,stop))
    try:
        session.start()
        sql('CREATE TABLE t(id BIGINT PRIMARY KEY,s VARCHAR(2048),v VECTOR(2))')
        sql('BEGIN')
        before=time.perf_counter()
        for start in range(1,args.rows+1,100):
            stop=min(start+100,args.rows+1)
            sql('INSERT INTO t VALUES '+batch(start,stop),affected_rows=stop-start)
        assert exists()
        payload=size();assert payload>2*2**30,('spilled payload below formal budget',payload)
        record('uncommitted load',payload_bytes=payload,seconds=round(time.perf_counter()-before,3))
        bad=session.request('INSERT INTO t VALUES '+batch(args.rows+1,args.rows+5)+",(1,'dup','[0,0]')")
        assert bad.get('ok') is False,bad
        assert size()==payload,'failed statement leaked spill bytes'
        sql('SELECT id FROM t WHERE id=%d'%(args.rows+1),rows=[])
        sql("UPDATE t SET s='updated' WHERE id=1",affected_rows=1)
        sql('SELECT s FROM t WHERE id=1',rows=[['updated']])
        before=time.perf_counter();sql('COMMIT')
        record('large commit',seconds=round(time.perf_counter()-before,3))
        assert not exists()
        session.crash();session.start()
        sql('SELECT id,s FROM t WHERE id=1',rows=[[1,'updated']])
        sql('SELECT id,s FROM t WHERE id=%d'%args.rows,rows=[[args.rows,'x']])
        sql("DELETE FROM t WHERE s='x'",affected_rows=args.rows-1)
        sql('SELECT id FROM t',rows=[[1]])
        # Abandoned uncommitted spill is discarded on startup.
        sql('BEGIN')
        for start in range(args.rows+1,args.rows+2001,100):
            sql('INSERT INTO t VALUES '+batch(start,start+100),affected_rows=100)
        assert exists()
        session.crash();session.start();assert not exists()
        sql('SELECT id FROM t',rows=[[1]])
        session.shutdown()
        output.write_text(json.dumps(dict(passed=True,stages=stages),indent=2)+'\n',encoding='utf-8')
        print('Docker long transaction spill: PASS',flush=True)
    except Exception as error:
        state=session.docker('inspect','--format','{{json .State}}',session.name,check=False)
        output.write_text(json.dumps(dict(passed=False,error=str(error),docker_state=state.stdout.decode(errors='replace'),stages=stages),indent=2),encoding='utf-8')
        print(output.read_text(encoding='utf-8'),flush=True)
        raise
    finally:session.close()
if __name__=='__main__':main()
