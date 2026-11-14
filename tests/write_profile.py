"""Development-only CPU profile; not an official performance measurement."""
import argparse
import json
import os
from pathlib import Path
import time
from client import Client
from paired_reference_perf import corpus

p=argparse.ArgumentParser();p.add_argument('--output',required=True);p.add_argument('--binary',required=True);args=p.parse_args()
out=Path(args.output).resolve();out.mkdir(parents=True,exist_ok=False)
binary=Path(args.binary).resolve();data=corpus(91271,16,queries=1)
records=[]
for round in range(8):
    directory=out/str(round);directory.mkdir();os.chdir(directory)
    db=Client(directory/'data',command=[str(binary),'--data-dir',str(directory/'data')],timeout=90)
    try:
        db.execute('CREATE TABLE docs(id BIGINT PRIMARY KEY,category VARCHAR(16),content VARCHAR(1024),embedding VECTOR(128));')
        start=time.perf_counter()
        for sql in data['inserts']:
            db.execute('BEGIN;');db.execute(sql);db.execute('COMMIT;')
        elapsed=time.perf_counter()-start
        assert db.execute('SELECT id FROM docs ORDER BY id;')['rows']==[[i] for i in range(10000)]
        db.shutdown();assert (directory/'gmon.out').is_file()
        records.append({'round':round,'profiled_wall_seconds':elapsed})
    finally:db.kill()
(out/'report.json').write_text(json.dumps({'passed':True,'scope':'Instrumented diagnostic only, not a comparable benchmark.','rounds':records},indent=2))
