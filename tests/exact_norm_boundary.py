"""Independent Decimal oracle for squared-norm rounding at float64 midpoints."""
import argparse,decimal,json,math,struct,sys,tempfile
from pathlib import Path
sys.path.insert(0,str(Path(__file__).resolve().parent))
from protocol_client import create_fixture_client
p=argparse.ArgumentParser();p.add_argument('--output',type=Path);p.add_argument('--image');a=p.parse_args()
decimal.getcontext().prec=600
report=dict(passed=False,requests=0,scales=0);last={}
def f32(x):return struct.unpack('f',struct.pack('f',x))[0]
def truth(v,q):return math.sqrt(float(sum((decimal.Decimal.from_float(x)-decimal.Decimal.from_float(y))**2 for x,y in zip(v,q))))
try:
    with tempfile.TemporaryDirectory(prefix='mvd-exact-norm-') as directory:
        c=create_fixture_client(directory,a.image,a.output)
        try:
            def sql(text,**fields):
                global last
                r=c.request(text);report['requests']+=1;last=dict(sql=text,response=r)
                assert r['ok'],last
                for k,v in fields.items():assert r.get(k)==v,(last,k,v)
                return r
            # Keep the cancellation term normal in float32 at every scale.
            for power in [-40,-20,0,20,80]:
                scale=2.0**power;x=f32(2.0**-20*scale)
                bits=struct.unpack('I',struct.pack('f',x))[0]
                down=struct.unpack('f',struct.pack('I',bits-1))[0]
                q=[f32((9*2.0**-69+2.0**-89)*scale),f32(-3*2.0**-34*scale)]
                rows={1:[f32(scale),x],2:[f32(scale),down]}
                expected=[[ident,truth(vector,q)] for ident,vector in rows.items()]
                assert expected[0][1]==expected[1][1],expected
                sql('CREATE TABLE t(id BIGINT PRIMARY KEY,v VECTOR(2))')
                sql('INSERT INTO t VALUES '+','.join("(%d,'%s')"%(i,json.dumps(v)) for i,v in rows.items()))
                expr="L2_DISTANCE(v,'%s')"%json.dumps(q)
                for indexed in [False,True]:
                    if indexed:sql('CREATE VECTOR INDEX vi ON t(v)')
                    for order in ['',' LIMIT 1',' LIMIT 2',' DESC LIMIT 2']:
                        wanted=expected[:1] if order==' LIMIT 1' else expected
                        sql('SELECT id,'+expr+' AS d FROM t ORDER BY d'+order,rows=wanted)
                c.kill();c.start()
                sql('SELECT id,'+expr+' AS d FROM t ORDER BY d LIMIT 2',rows=expected)
                sql('DROP TABLE t');report['scales']+=1
            report.update(passed=True,exact_decimal_squared_oracle=True,heap_generic_index_restart=True)
        finally:c.close()
except Exception:
    report['failure']=last;raise
finally:
    if a.output:
        a.output.parent.mkdir(parents=True,exist_ok=True);a.output.write_text(json.dumps(report,ensure_ascii=False,indent=2)+'\n',encoding='utf-8')
    print(json.dumps(report,ensure_ascii=False),flush=True)
