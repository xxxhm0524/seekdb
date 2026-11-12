#!/usr/bin/env python3
"""Compare locked old/new Docker sources using the untouched official transport."""
import argparse
import hashlib
import json
from pathlib import Path
import subprocess
import sys

ROOT=Path(__file__).resolve().parent.parent
parser=argparse.ArgumentParser()
parser.add_argument('--official-root',type=Path,required=True)
parser.add_argument('--reference-image',required=True)
parser.add_argument('--candidate-image',required=True)
parser.add_argument('--output',type=Path,required=True)
args=parser.parse_args()
official=args.official_root.resolve();output=args.output.resolve();output.mkdir(parents=True,exist_ok=False)
OFFICIAL='f4d4e26653af823a7601668802f80e374ad6643c'
REFERENCE='6da81bf4b562c6bb055839e30b09e3f90c615bf2'
CANDIDATE='387cda5a264d338ac446536f7433e4557c0c472d'
report=dict(passed=False,official_sha=OFFICIAL,reference_sha=REFERENCE,candidate_sha=CANDIDATE,
    scope='Unmodified public checker and local syntax counterexample; not the unpublished formal 29 cases.',
    formal_29_verified=False,formal_failure_cause_proven=False,public_checks={},bare_vector_checks={})

def run(command,log):
    process=subprocess.run(command,cwd=ROOT,stdout=subprocess.PIPE,stderr=subprocess.STDOUT,timeout=900)
    log.write_bytes(process.stdout)
    if process.returncode:
        print(process.stdout[-2500:].decode(errors='replace'),flush=True)
    return process.returncode

try:
    names=subprocess.check_output(['git','ls-tree','-r','--name-only',OFFICIAL,
        'STDIO_PROTOCOL.md','protocol_check','mysqltest'],cwd=ROOT,text=True).splitlines()
    hashes={}
    for name in names:
        wanted=subprocess.check_output(['git','show',OFFICIAL+':'+name],cwd=ROOT)
        actual=(official/name).read_bytes()
        assert actual==wanted,('Official file changed',name)
        hashes[name]=hashlib.sha256(actual).hexdigest()
    report['official_package_files_verified']=len(hashes)
    report['official_package_sha256']=hashes
    config=json.loads((official/'protocol_check/config.json').read_text())
    report['resources']=config
    for version,image in [('reference',args.reference_image),('candidate',args.candidate_image)]:
        destination=output/(version+'-public')
        command=[sys.executable,str(official/'protocol_check/run.py'),'--image',image,
            '--package',str(official/'mysqltest'),'--config',str(official/'protocol_check/config.json'),
            '--output',str(destination)]
        code=run(command,output/(version+'-public.log'))
        value=json.loads((destination/'result.json').read_text()) if (destination/'result.json').exists() else {}
        passed=code==0 and value.get('passed') is True and len(value['cases'])==20
        report['public_checks'][version]=dict(passed=passed,exit_code=code,cases=len(value.get('cases',[])))
        assert passed,(version,'Unmodified official public checker failed')
    sys.path.insert(0,str(official/'protocol_check'))
    from _transport import Session
    for version,image in [('reference',args.reference_image),('candidate',args.candidate_image)]:
        session=Session(config,output/(version+'-bare.stderr.log'),image=image)
        expect_success=version=='candidate'
        def sql(text,**wanted):
            response=session.request(text)
            assert response['ok'] is True,(text,response)
            for key,value in wanted.items():assert response.get(key)==value,(text,response,value)
            return response
        try:
            session.start()
            sql('CREATE TABLE t(id BIGINT PRIMARY KEY,v VECTOR(2))')
            sql("INSERT INTO t VALUES(1,'[1,2]'),(2,'[-1,2]')",affected_rows=2)
            sql('INSERT INTO t VALUES(10,[1,2])',affected_rows=1)
            sql("SELECT id,L2_DISTANCE(v,'[-1,2]') AS d FROM t ORDER BY d LIMIT 1",rows=[[2,0]])
            probes={}
            for name,text in [('insert','INSERT INTO t VALUES(3,[-1,2])'),
                ('update','UPDATE t SET v=[-3,+4] WHERE id=1'),
                ('query','SELECT id,L2_DISTANCE(v,[-1,2]) AS d FROM t ORDER BY d LIMIT 1')]:
                response=session.request(text)
                probes[name]=dict(sql=text,response=response)
                assert response['ok'] is expect_success,(version,name,response)
                if expect_success:
                    if name=='query':assert response['rows']==[[2,0]],response
                    else:assert response['affected_rows']==1,response
                else:assert response.get('error',{}).get('code')=='invalid_vector',response
            sql('SELECT v FROM t WHERE id=1',rows=[[[(-3 if expect_success else 1),(4 if expect_success else 2)]]])
            sql('SELECT id FROM t WHERE id=3',rows=[[3]] if expect_success else [])
            sql("SELECT id,L2_DISTANCE(v,'[-1,2]') AS d FROM t ORDER BY d LIMIT 1",rows=[[2,0]])
            session.shutdown()
            report['bare_vector_checks'][version]=dict(passed=True,positive_bare_control=True,
                negative_quoted_control=True,expected_bare_success=expect_success,probes=probes)
        finally:session.close()
    code=run([sys.executable,str(ROOT/'tests/vector_literal_syntax.py'),
        '--image',args.candidate_image,'--protocol-root',str(official/'protocol_check'),
        '--output',str(output/'vector-literal-final-runtime.json')],output/'vector-literal-final-runtime.log')
    full=json.loads((output/'vector-literal-final-runtime.json').read_text()) if (output/'vector-literal-final-runtime.json').exists() else {}
    assert code==0 and full.get('passed') and full.get('checks')==1036,full
    report['candidate_full_syntax_regression']=full
    report['passed']=True
except Exception as error:
    report['error_type']=type(error).__name__
    report['error']=str(error)[:3000]
    raise
finally:
    (output/'summary.json').write_text(json.dumps(report,ensure_ascii=False,indent=2)+'\n',encoding='utf-8')
    print(json.dumps({key:value for key,value in report.items() if key!='official_package_sha256'},ensure_ascii=False),flush=True)
