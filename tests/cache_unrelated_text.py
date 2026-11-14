"""A fitting vector working set must not be displaced by unrelated body text."""
import argparse
import json
import os
from pathlib import Path
import subprocess
from client import Client, ROOT

def run(directory,shim):
    assert os.name=='posix' and not os.environ.get('MVD_DOCKER_IMAGE')
    db=Client(directory,timeout=60,env={'MVD_VECTOR_CACHE_MIB':'16'})
    count,dimensions=10000,128
    query="SELECT id,L2_DISTANCE(v,'["+','.join('0' for _ in range(dimensions))+"]') AS d FROM t WHERE category='c3' ORDER BY d LIMIT 10;"
    expected=sorted((k for k in range(count) if k%7==3),key=lambda k:((k%29)**2,k))[:10]
    try:
        db.execute(f'CREATE TABLE t(id BIGINT PRIMARY KEY,category VARCHAR(16),content VARCHAR(1024),v VECTOR({dimensions}));')
        for base in range(0,count,100):
            values=','.join(f"({k},'c{k%7}','{'x'*1024}','["+','.join(str(k%29) for _ in range(dimensions))+" ]')" for k in range(base,base+100))
            db.execute('INSERT INTO t VALUES'+values+';')
        db.execute('CREATE VECTOR INDEX idx ON t(v);');db.shutdown()
        report=directory/'cache-pread.json'
        db.env.update(LD_PRELOAD=str(shim),MVD_IO_REPORT=str(report));db.start()
        for _ in range(20):
            rows=db.execute(query)['rows'];assert [r[0] for r in rows]==expected
        db.shutdown()
        read_bytes=json.loads(report.read_text())['database_read_bytes']
        assert read_bytes<16*1024*1024,f'16 MiB cache reread {read_bytes} bytes for a fitting vector/filter working set with unrelated 1024-byte text'
        for name in ['LD_PRELOAD','MVD_IO_REPORT']:db.env.pop(name,None)
        db.start()
        assert [r[0] for r in db.execute(query)['rows']]==expected
        # A large filter can refuse cache admission; the next small-filter query
        # and an uncached winner projection must still return current values.
        large_query=query.replace("category='c3'","content='"+'x'*1024+"'")
        all_expected=sorted(range(count),key=lambda k:((k%29)**2,k))[:10]
        assert [r[0] for r in db.execute(large_query)['rows']]==all_expected
        assert [r[0] for r in db.execute(query)['rows']]==expected
        with_body=query.replace('SELECT id,','SELECT id,content,')
        assert all(row[1]=='x'*1024 for row in db.execute(with_body)['rows'])
        db.execute('BEGIN;');db.execute(f"UPDATE t SET content='pending' WHERE id={expected[0]};")
        assert db.execute(with_body)['rows'][0][1]=='pending'
        db.execute('ROLLBACK;');assert db.execute(with_body)['rows'][0][1]=='x'*1024
        return dict(passed=True,rows=count,dimensions=dimensions,cache_mib=16,body_bytes_per_row=1024,
                    repeated_queries=20,file_read_bytes=read_bytes,projection_and_refusal_transitions=True)
    finally:db.kill()

if __name__=='__main__':
    parser=argparse.ArgumentParser();parser.add_argument('--output',required=True);args=parser.parse_args()
    out=Path(args.output).resolve();out.mkdir(parents=True,exist_ok=False);shim=out/'io-probe.so'
    subprocess.run(['cc','-shared','-fPIC','-O2',str(ROOT/'tests/io_probe.c'),'-o',str(shim),'-ldl'],check=True)
    try:result=run(out/'data',shim)
    except Exception as error:result=dict(passed=False,error=str(error))
    (out/'report.json').write_text(json.dumps(result,indent=2),encoding='utf-8');print(json.dumps(result),flush=True)
    raise SystemExit(0 if result['passed'] else 1)
