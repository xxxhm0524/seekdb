"""Malformed JSON with a raw NUL must never execute its valid prefix."""
import argparse
import json
from pathlib import Path
import sys
from client import Client

def run(directory):
    db=Client(directory)
    try:
        request=dict(id=1,op='execute',sql='CREATE TABLE phantom(id BIGINT PRIMARY KEY);')
        db.process.stdin.write(json.dumps(request).encode()+b'\x00trailing\n');db.process.stdin.flush()
        response=db.receive()
        assert response.get('ok') is False,'raw NUL terminated parsing early and executed a JSON prefix'
        assert response.get('error',{}).get('code')=='invalid_request'
        db.execute('SELECT id FROM phantom;',ok=False)
        db.execute('CREATE TABLE good(id BIGINT PRIMARY KEY,s VARCHAR(8));')
        db.execute("INSERT INTO good VALUES(7,'a\x00b');")
        assert db.execute('SELECT s FROM good;')['rows']==[['a\x00b']]
        return dict(passed=True,malformed_prefix_not_executed=True,escaped_nul_string_data_preserved=True)
    finally:db.kill()

if __name__=='__main__':
    sys.stdout.reconfigure(encoding='utf-8')
    p=argparse.ArgumentParser();p.add_argument('--output',required=True);args=p.parse_args()
    out=Path(args.output);out.mkdir(parents=True,exist_ok=False)
    try:result=run(out/'data')
    except Exception as error:result=dict(passed=False,error=str(error))
    (out/'report.json').write_text(json.dumps(result,indent=2),encoding='utf-8');print(json.dumps(result),flush=True)
    sys.exit(0 if result['passed'] else 1)
