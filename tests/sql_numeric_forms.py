"""SQL decimal spellings accepted by the successful version, with own truth."""
import argparse
import json
import math
from pathlib import Path
import struct
import sys
from client import Client, vector_bits


def f32(value): return struct.unpack('<f',struct.pack('<f',value))[0]


def run(directory):
    maximum=struct.unpack('<f',bytes.fromhex('ffff7f7f'))[0]
    forms=[('0',0.0),('-0',-0.0),('+0',0.0),('1',1.0),('+1',1.0),('-1',-1.0),
           ('.5',.5),('-.5',-.5),('+.5',.5),('1.',1.0),('-1.',-1.0),('+1.',1.0),
           ('01',1.0),('001.25',1.25),('1e+2',100.0),('+1e-2',f32(.01)),('1.e2',100.0),
           ('.125e+2',12.5),('1E-9999',0.0),('-1E-9999',-0.0),('3.4028235e38',maximum),('-3.4028235e38',-maximum)]
    db=Client(directory,timeout=90);checked=0
    try:
        db.execute('CREATE TABLE t(id BIGINT PRIMARY KEY,txt VARCHAR(+32),v VECTOR(+3));')
        key=0
        for spelling,value in forms:
            for quoted in [False,True]:
                literal='['+spelling+',0,0]';literal="'"+literal+"'" if quoted else literal
                db.execute(f"INSERT INTO t VALUES({key},'number',{literal});")
                row=db.execute(f'SELECT v FROM t WHERE id={key};')['rows'][0][0]
                assert vector_bits([row[0]])==vector_bits([value]),(spelling,row,value)
                expected=math.sqrt(float(value)*float(value))
                result=db.execute(f"SELECT L2_DISTANCE(v,'[0,0,0]') AS d FROM t WHERE id={key} ORDER BY d LIMIT +1;")['rows']
                assert len(result)==1 and math.isclose(result[0][0],expected,rel_tol=1e-14,abs_tol=0)
                db.execute(f'UPDATE t SET v={literal} WHERE id={key};')
                key+=1;checked+=3
        for literal in ["'[1,\f2,3]'","'[1/*comment*/,2,3]'","'[1,--line\n2,3]'","'/*prefix*/[1,2,3]/*suffix*/'","[1/*comment*/,2,3]"]:
            db.execute(f"INSERT INTO t VALUES({key},'comments',{literal});")
            assert db.execute(f'SELECT v FROM t WHERE id={key};')['rows']==[[[1.0,2.0,3.0]]]
            key+=1;checked+=1
        for limit in ['0','-0','-000','+0']:
            assert db.execute('SELECT id FROM t LIMIT '+limit+';')['rows']==[];checked+=1
        invalid=['[1 2,0,0]','[1/**/2,0,0]','[1,0,]','["1",0,0]','[NaN,0,0]','[Infinity,0,0]',
                 '[++,0,0]','[.,0,0]','[1e,0,0]','[1e +2,0,0]','[+ 1,0,0]','[1e9999,0,0]',
                 '[1,2,3]junk','[1,2,3];','[1,2,3]\x00junk','[1/*unterminated,2,3]']
        before=db.execute('SELECT id FROM t;')['rows']
        for text in invalid:
            escaped=text.replace('\\','\\\\').replace("'","''")
            db.execute(f"INSERT INTO t VALUES(9999,'bad','{escaped}');",ok=False)
            assert db.execute('SELECT id FROM t;')['rows']==before;checked+=1
        db.execute('BEGIN;');db.execute("INSERT INTO t VALUES(10000,'kept','[+.5,1.,01]');")
        db.execute("INSERT INTO t VALUES(10001,'partial','[+1,.5,1.]'),(10002,'bad','[1 2,0,0]');",ok=False)
        db.execute('COMMIT;');db.restart(crash=True)
        assert db.execute('SELECT id,v FROM t WHERE id>=10000;')['rows']==[[10000,[.5,1.0,1.0]]]
        return dict(passed=True,production_checks=checked,sql_number_spellings=len(forms),quoted_and_unquoted=True,
                    formfeed_and_comments=True,malformed_rejected=len(invalid),float32_bits_checked=True,late_error_and_restart=True)
    finally:db.kill()


if __name__=='__main__':
    sys.stdout.reconfigure(encoding='utf-8')
    p=argparse.ArgumentParser();p.add_argument('--output',required=True);args=p.parse_args()
    out=Path(args.output);out.mkdir(parents=True,exist_ok=False)
    try:result=run(out/'data')
    except Exception as error:
        import traceback
        result=dict(passed=False,error=str(error),traceback=traceback.format_exc())
    (out/'report.json').write_text(json.dumps(result,indent=2),encoding='utf-8');print(json.dumps(result),flush=True)
    raise SystemExit(0 if result['passed'] else 1)
