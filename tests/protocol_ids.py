"""Independent arbitrary-precision ID oracle across every response path."""
import argparse
import json
import math
from pathlib import Path
import sys
from client import Client


def typed(value):
    if isinstance(value, dict): return ('object', sorted((k, typed(v)) for k, v in value.items()))
    if isinstance(value, list): return ('array', [typed(v) for v in value])
    return (type(value).__name__, value)


def run(out):
    db = Client(out / 'data', timeout=90); checked = 0
    ids = [None, True, False, 0, -1, -(2**63), 2**63-1, 2**64-1, 2**64, 2**64+1, -(2**63)-1,
           10**100, -(10**100), 10**400, 10**3000, '汉😀\0id', ['id', 2**64+1], {'n':10**400,'list':[True,None,'é']}, .125, -0.0,
           'large-id-' + 'x'*300000]
    def send(identifier, op='execute', sql='SELECT id FROM t;', ok=True, **extra):
        nonlocal checked
        value=dict(id=identifier,op=op,**extra)
        if op=='execute':value['sql']=sql
        raw=(json.dumps(value,ensure_ascii=False,allow_nan=False)+'\n').encode()
        db.process.stdin.write(raw);db.process.stdin.flush();response=db.receive()
        assert typed(response.get('id'))==typed(identifier), (identifier,response)
        assert response['ok'] is ok, response
        checked+=1;return response
    try:
        db.execute('CREATE TABLE t(id BIGINT PRIMARY KEY,txt VARCHAR(65536));')
        for identifier in ids:
            assert send(identifier)['rows']==[]
            send(identifier,sql='SELECT missing FROM t;',ok=False)
        send({'large':10**400},sql='BEGIN;')
        send(10**400,sql="INSERT INTO t VALUES(1,'kept');")
        send(10**3000,sql="INSERT INTO t VALUES(2,'partial'),(1,'duplicate');",ok=False)
        send(['commit',10**400],sql='COMMIT;')
        db.restart(crash=True)
        assert send({'after_restart':10**400})['rows']==[[1]]
        # Large spooled response has to preserve the same raw ID contract.
        db.execute("INSERT INTO t VALUES"+','.join(f"({i},'{'z'*65536}')" for i in range(2,8))+';')
        rows=send({'spooled':10**400},sql='SELECT txt FROM t ORDER BY id;')['rows']
        assert rows==[['kept']]+[['z'*65536] for _ in range(6)]
        # Malformed trailing input must reject before executing its SQL prefix.
        raw=b'{"id":18446744073709551617,"op":"execute","sql":"DELETE FROM t;"}\x00junk\n'
        db.process.stdin.write(raw);db.process.stdin.flush();assert db.receive()['ok'] is False
        assert len(send(10**100)['rows'])==7
        send({'shutdown':10**3000},op='shutdown');assert db.process.wait(timeout=10)==0
        db.start();assert len(send(10**400)['rows'])==7
        return dict(passed=True,requests_checked=checked,maximum_integer_digits=3001,large_string_id_bytes=300009,
                    success_error_commit_shutdown_and_spooled=True,statement_failure_atomic=True,raw_nul_not_executed=True)
    finally:db.kill()


if __name__=='__main__':
    sys.stdout.reconfigure(encoding='utf-8')
    p=argparse.ArgumentParser();p.add_argument('--output',required=True);args=p.parse_args()
    out=Path(args.output).resolve();out.mkdir(parents=True,exist_ok=False)
    try:result=run(out)
    except Exception as error:
        import traceback
        result=dict(passed=False,error=str(error),traceback=traceback.format_exc())
    (out/'report.json').write_text(json.dumps(result,ensure_ascii=False,indent=2),encoding='utf-8');print(json.dumps(result,ensure_ascii=False),flush=True)
    raise SystemExit(0 if result['passed'] else 1)
