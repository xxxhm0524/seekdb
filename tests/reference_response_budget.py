"""Actual protocol byte budget, with float32-bit rather than double-text truth."""
import argparse
import ctypes
import json
import math
from pathlib import Path
import random
import struct
import sys
from client import Client, CONFIG


class MeasuredClient(Client):
    def receive(self):
        line=self.lines.get(timeout=self.timeout)
        assert line is not None,'database exited before a response'
        self.last_response_bytes=len(line)
        return json.loads(line,parse_constant=lambda x:(_ for _ in ()).throw(ValueError(x)))


def bits(value):
    assert type(value) in (int,float) and math.isfinite(value)
    return struct.pack('<f',ctypes.c_float(value).value)


def run(out,reference_binary,ours_binary=None):
    count,dimensions=10000,128
    rng=random.Random(731901);expected=[];statements=[]
    for base in range(0,count,100):
        values=[]
        for key in range(base,base+100):
            vector=[struct.unpack('<f',struct.pack('<f',rng.uniform(-1,1)))[0] for _ in range(dimensions)]
            expected.append(b''.join(struct.pack('<f',x) for x in vector))
            values.append(f"({key},'{json.dumps(vector,separators=(',',':'))}')")
        statements.append('INSERT INTO t VALUES'+','.join(values)+';')
    results={}
    for name,binary in [('ours',ours_binary),('reference',reference_binary)]:
        directory=out/name
        command=None if binary is None else [str(binary),*(['--protocol','jsonl'] if name=='reference' else []),'--data-dir',str(directory)]
        db=MeasuredClient(directory,command=command,timeout=90)
        try:
            db.execute(f'CREATE TABLE t(id BIGINT PRIMARY KEY,v VECTOR({dimensions}));')
            for sql in statements:db.execute(sql)
            response=db.execute('SELECT id,v FROM t ORDER BY id;')
            size=db.last_response_bytes
            assert response['columns']==['id','v'] and len(response['rows'])==count
            for key,row in enumerate(response['rows']):
                assert type(row[0]) is int and row[0]==key and len(row[1])==dimensions
                assert b''.join(bits(x) for x in row[1])==expected[key],(name,key)
            results[name]=dict(ok=True,response_bytes=size,within_official_limit=size<=CONFIG['max_response_bytes'],float32_bits_roundtrip=True)
            del response
        finally:db.kill()
    return dict(passed=all(x['within_official_limit'] for x in results.values()),rows=count,dimensions=dimensions,
                configured_max_response_bytes=CONFIG['max_response_bytes'],actors=results,
                scope='Complete actual JSONL responses, checked with Python arbitrary-precision JSON and hardware float32 conversion; not an official score.')


if __name__=='__main__':
    p=argparse.ArgumentParser();p.add_argument('--output',required=True);p.add_argument('--reference-bin',required=True);p.add_argument('--ours-bin');args=p.parse_args()
    out=Path(args.output).resolve();out.mkdir(parents=True,exist_ok=False)
    try:result=run(out,Path(args.reference_bin).resolve(),Path(args.ours_bin).resolve() if args.ours_bin else None)
    except Exception as error:
        import traceback
        result=dict(passed=False,error=str(error),traceback=traceback.format_exc())
    (out/'report.json').write_text(json.dumps(result,indent=2),encoding='utf-8');print(json.dumps(result),flush=True)
    raise SystemExit(0 if result['passed'] else 1)
