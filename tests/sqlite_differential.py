"""Independent SQL/transaction oracle; vector scalar uses stored float32 values."""
import argparse,hashlib,json,math,random,sqlite3,struct,subprocess,sys,tempfile
from pathlib import Path
sys.path.insert(0,str(Path(__file__).resolve().parent))
from protocol_client import Client
p=argparse.ArgumentParser();p.add_argument('--seed',type=int,default=20261116);p.add_argument('--steps',type=int,default=1200);p.add_argument('--wide-vectors',action='store_true');p.add_argument('--output',type=Path);a=p.parse_args()
rng=random.Random(a.seed);report=dict(passed=False,seed=a.seed,steps=a.steps,requests=0,query_comparisons=0,restarts=0);last={}
root=Path(__file__).resolve().parent.parent
report.update(numeric_scope='exploratory wide-exponent oracle comparison' if a.wide_vectors else 'small integer float32 vectors: squared terms and sums exactly representable in float64',
              base_sha=subprocess.check_output(['git','rev-parse','HEAD'],cwd=root,text=True).strip(),
              source_sha256={name:hashlib.sha256((root/name).read_bytes()).hexdigest() for name in ['src/main.cpp','src/sql.cpp','src/sql.h','src/engine.cpp']})
def literal(x):
    if isinstance(x,str):return "'"+x.replace('\\','\\\\').replace("'","''")+"'"
    return str(x)
def vector():return [struct.unpack('f',struct.pack('f',rng.choice([-3,-1,0,1,3,1e-30,1e8] if a.wide_vectors else [-3,-1,0,1,3])))[0] for _ in range(9)]
def distance(x,y):return math.sqrt(math.fsum((float(i)-float(j))**2 for i,j in zip(json.loads(x),json.loads(y))))
try:
    with tempfile.TemporaryDirectory(prefix='mvd-sqlite-') as directory:
        c=Client(datadir=directory,extra=['--cache-mb=1','--tx-memory-mb=1','--sort-memory-mb=1'])
        db=sqlite3.connect(':memory:',isolation_level=None);db.create_function('L2_DISTANCE',2,distance)
        try:
            def execute(sql,reference=None,params=(),query=False):
                global last
                reference=reference or sql
                cur=None;reference_error=False
                try:cur=db.execute(reference,params)
                except sqlite3.Error:reference_error=True
                result=c.request(sql,'exact');report['requests']+=1
                last=dict(sql=sql,reference_sql=reference,params=params,response=result,reference_error=reference_error)
                assert result['ok'] is (not reference_error),last
                if reference_error:return
                if query:
                    report['reference_snapshot']=[list(row) for row in db.execute('SELECT pk,s,v FROM t ORDER BY pk')]
                    want=[list(row) for row in cur.fetchall()];actual=result['rows']
                    last['expected']=want
                    assert len(actual)==len(want),last
                    for left,right in zip(actual,want):
                        assert len(left)==len(right),last
                        for v,w in zip(left,right):
                            if isinstance(w,float):assert math.isclose(v,w,rel_tol=1e-12,abs_tol=1e-40),last
                            else:assert v==w,last
                    assert result['columns']==[col[0] for col in cur.description],last
                    report['query_comparisons']+=1
                elif 'affected_rows' in result:assert result['affected_rows']==cur.rowcount,last
            execute('CREATE TABLE t(g BIGINT,s VARCHAR(40),v VECTOR(9),pk BIGINT PRIMARY KEY,n BIGINT)',
                    'CREATE TABLE t(g INTEGER,s TEXT,v TEXT,pk INTEGER PRIMARY KEY,n INTEGER)')
            active=False
            for step in range(a.steps):
                op=rng.randrange(100)
                if op<7:
                    if active:execute(rng.choice(['COMMIT','ROLLBACK']));active=False
                    else:execute('BEGIN');active=True
                elif op<10:
                    c.kill();c.start();report['restarts']+=1
                    if active:db.execute('ROLLBACK');active=False
                elif op<40:
                    pk=rng.choice([-(2**63),2**63-1]+list(range(-80,81)))
                    values=[rng.randrange(-3,4),rng.choice(['','a','aa','中','😀',"a'b",'x\\y','\n','\0z']),json.dumps(vector()),pk,rng.randrange(-7,8)]
                    execute('INSERT INTO t VALUES('+','.join(literal(x) for x in values)+')','INSERT INTO t VALUES(?,?,?,?,?)',values)
                elif op<61:
                    col=rng.choice(['g','s','n','v']);value=json.dumps(vector()) if col=='v' else rng.choice(['','x','中','😀',"x'y",'\0z']) if col=='s' else rng.randrange(-8,9)
                    threshold=rng.randrange(-80,81)
                    execute('UPDATE t SET '+col+'='+literal(value)+' WHERE pk<='+str(threshold),'UPDATE t SET '+col+'=? WHERE pk<=?',(value,threshold))
                elif op<70:
                    threshold=rng.randrange(-80,81);execute('DELETE FROM t WHERE pk>='+str(threshold))
                else:
                    operator=rng.choice(['=','<','<=','>','>=','!='])
                    col=rng.choice(['s','g','pk','n']);value=rng.choice(['','a','aa','中','中😀','😀','\0z']) if col=='s' else rng.randrange(-8,9)
                    where=' WHERE '+col+operator
                    limit=rng.choice([0,1,3,11,500]);offset=rng.choice([0,1,3,300])
                    direction=rng.choice(['ASC','DESC'])
                    if rng.randrange(2):
                        q=json.dumps(vector());head='SELECT pk,g,s,L2_DISTANCE(v,'
                        tail=') AS d FROM t'+where
                        end=' ORDER BY d '+direction+',s '+direction+',pk ASC LIMIT %d OFFSET %d'%(limit,offset)
                        execute(head+literal(q)+tail+literal(value)+end,head+'?'+tail+'?'+end,(q,value),True)
                    else:
                        head='SELECT pk,g,s,n FROM t'+where
                        end=' ORDER BY s '+direction+',n '+direction+',pk ASC LIMIT %d OFFSET %d'%(limit,offset)
                        execute(head+literal(value)+end,head+'?'+end,(value,),True)
                # This catches a failed statement leaving partial mutations even before COMMIT.
                execute('SELECT pk,g,s,n FROM t ORDER BY pk',query=True)
            if active:execute('COMMIT')
            c.kill();c.start();report['restarts']+=1
            execute('SELECT pk,g,s,n FROM t ORDER BY pk',query=True)
            report['passed']=True
        finally:c.close();db.close()
except Exception:
    report['failure']=last;raise
finally:
    if a.output:
        a.output.parent.mkdir(parents=True,exist_ok=True);a.output.write_text(json.dumps(report,ensure_ascii=False,indent=2)+'\n',encoding='utf-8')
    print(json.dumps({k:v for k,v in report.items() if k not in ['reference_snapshot']},ensure_ascii=False),flush=True)
