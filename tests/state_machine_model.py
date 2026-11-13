"""Independent state-machine oracle for the required SQL/transaction subset."""
import argparse
from collections import Counter
import copy
import functools
import hashlib
import json
import math
from pathlib import Path
import random
import subprocess
import sys
import tempfile

ROOT=Path(__file__).resolve().parent.parent
sys.path.insert(0,str(ROOT/'tests'))
from protocol_client import Client,create_fixture_client

parser=argparse.ArgumentParser()
parser.add_argument('--seed',type=int,default=20261112)
parser.add_argument('--steps',type=int,default=1000)
parser.add_argument('--wide-spill',action='store_true')
parser.add_argument('--image')
parser.add_argument('--output',type=Path)
args=parser.parse_args();rng=random.Random(args.seed)
report=dict(passed=False,seed=args.seed,steps=args.steps,
    base_sha=subprocess.check_output(['git','rev-parse','HEAD'],cwd=ROOT,text=True).strip(),
    working_tree_dirty=bool(subprocess.check_output(['git','status','--porcelain'],cwd=ROOT,text=True).strip()),
    source_sha256={str(path.relative_to(ROOT)).replace('\\','/'):hashlib.sha256(path.read_bytes()).hexdigest() for path in sorted((ROOT/'src').rglob('*')) if path.is_file() and path.suffix in ('.h','.cpp','.mk')},
    script_sha256=hashlib.sha256(Path(__file__).read_bytes()).hexdigest(),requests=0,
    scope='Independent Python state model; '+('Docker' if args.image else 'native')+' JSONL; no claim to reproduce the unpublished formal case.')
target=args.output or ROOT/'protocol-results'/('functional-state-holdout-%d-20261112'%args.seed)/'result.json'
out=target.parent/(target.stem+'.trace')
target.parent.mkdir(parents=True,exist_ok=True)
out.mkdir(exist_ok=False)
schemas={'a':dict(columns=['s','v','pk','g','n'],dim=9,width=384),
    'b':dict(columns=['n','pk','s','g','v'],dim=33,width=64),
    'c':dict(columns=['g','n','s','pk'],dim=0,width=4)}
if args.wide_spill:
    schemas['a']['width']=2048
    schemas['b']['width']=8184
report['wide_spill']=args.wide_spill
groups=['','a','b','甲','😀',"a'b",'\0x','z']
strings=['','a','甲','😀','é','e\u0301',"a'b",'a\\b','\n','\t','\0z','\U0010ffff']
model={name:{} for name in schemas};committed=copy.deepcopy(model);active=False
recalls={False:[],True:[]};operations=Counter();last={}

def literal(value):
    if isinstance(value,str):return "'"+value.replace('\\','\\\\').replace("'","''")+"'"
    return str(value)
def vliteral(vector):
    value='['+','.join(str(x) for x in vector)+']'
    return "'"+value+"'" if rng.randrange(2) else value
def make_row(name,ident):
    schema=schemas[name];text=rng.choice(strings)
    if len(text)>schema['width']:text=text[:schema['width']]
    if schema['width']>4 and rng.randrange(5)==0:text=rng.choice(['x','界','😀'])*schema['width']
    row=dict(pk=ident,g=rng.choice(groups),s=text,n=rng.randrange(-8,9))
    if schema['dim']:row['v']=tuple(rng.randrange(-3,4) for _ in range(schema['dim']))
    return row
def row_values(name,row):
    return '('+','.join(vliteral(row[col]) if col=='v' else literal(row[col]) for col in schemas[name]['columns'])+')'
def freeze(value):return tuple(freeze(x) for x in value) if isinstance(value,list) else value

try:
    with tempfile.TemporaryDirectory(prefix='mvd-state-holdout-') as data, (out/'requests.jsonl').open('w',encoding='utf-8') as trace:
        client=create_fixture_client(data,args.image,target) if args.image else Client(datadir=data,extra=['--cache-mb=1','--tx-memory-mb=1','--sort-memory-mb=1','--ann-min-rows=512'])
        try:
            def sql(text,mode='exact',expected_error=False,**expected):
                global last
                trace.write(json.dumps(dict(sql=text,mode=mode),ensure_ascii=False)+'\n');trace.flush()
                response=client.request(text,mode);report['requests']+=1
                last=dict(sql=text,mode=mode,response=response,expected=expected,active=active)
                assert response.get('ok') is (not expected_error),last
                if expected_error:assert response.get('error',{}).get('code'),last
                for key,value in expected.items():assert response.get(key)==value,last
                return response
            def snapshot():
                for name,schema in schemas.items():
                    rows=sql('SELECT * FROM '+name)['rows']
                    wanted=[[row[col] if col!='v' else list(row[col]) for col in schema['columns']] for row in model[name].values()]
                    assert Counter(map(freeze,rows))==Counter(map(freeze,wanted)),dict(name=name,actual=rows[:5],wanted=wanted[:5])
            def commit_model():
                global committed
                if not active:committed=copy.deepcopy(model)
            def where_for(name):
                kind=rng.randrange(5)
                if kind==0:return '',lambda row:True
                if kind==1:
                    g=rng.choice(groups+['absent']);return ' WHERE g='+literal(g),lambda row:row['g']==g
                if kind==2:
                    n=rng.randrange(-9,10);return ' WHERE n>='+str(n),lambda row:row['n']>=n
                if kind==3:
                    n=rng.randrange(-9,10);g=rng.choice(groups)
                    return ' WHERE n<='+str(n)+' AND g='+literal(g),lambda row:row['n']<=n and row['g']==g
                ident=rng.randrange(-500,1500);return ' WHERE pk='+str(ident),lambda row:row['pk']==ident
            def query(name):
                schema=schemas[name];where,predicate=where_for(name)
                eligible=[row for row in model[name].values() if predicate(row)]
                mode=rng.choice(['exact','ann']);limit=rng.choice([0,1,5,13,65,1000]);offset=rng.choice([0,1,10,2000])
                if schema['dim'] and rng.randrange(2):
                    vector=tuple(rng.randrange(-3,4) for _ in range(schema['dim']))
                    distance=lambda row:sum((x-y)**2 for x,y in zip(row['v'],vector))
                    descending=rng.randrange(5)==0
                    truth=sorted(eligible,key=lambda row:((-1 if descending else 1)*distance(row),row['pk']))[offset:offset+limit]
                    text="SELECT pk,g,L2_DISTANCE(v,%s) AS d FROM %s%s ORDER BY d %s LIMIT %d OFFSET %d"%(vliteral(vector),name,where,'DESC' if descending else 'ASC',limit,offset)
                    rows=sql(text,mode)['rows'];ids=[row[0] for row in rows];wanted=[row['pk'] for row in truth]
                    assert len(ids)==len(set(ids))==len(wanted),(last,wanted)
                    assert all(ident in model[name] and predicate(model[name][ident]) for ident in ids),last
                    for ident,g,distance_value in rows:
                        assert g==model[name][ident]['g'] and math.isclose(distance_value,math.sqrt(distance(model[name][ident])),rel_tol=1e-7,abs_tol=1e-7),last
                    keys=[((-1 if descending else 1)*distance(model[name][ident]),ident) for ident in ids]
                    assert keys==sorted(keys),last
                    if mode=='exact' or active or descending or offset or limit>64:
                        assert ids==wanted,(last,wanted)
                    elif wanted:recalls[bool(where)].append(len(set(ids)&set(wanted))/len(wanted))
                else:
                    desc=rng.choice([False,True]);keys=[('n',desc),('g',False)]
                    def compare(a,b):
                        for col,reverse in keys:
                            value=(a[col]>b[col])-(a[col]<b[col])
                            if value:return -value if reverse else value
                        return (a['pk']>b['pk'])-(a['pk']<b['pk'])
                    truth=sorted(eligible,key=functools.cmp_to_key(compare))[offset:offset+limit]
                    sql('SELECT pk,g,n,s FROM '+name+where+' ORDER BY n '+('DESC' if desc else 'ASC')+',g ASC LIMIT %d OFFSET %d'%(limit,offset),mode,
                        rows=[[row[col] for col in ['pk','g','n','s']] for row in truth])
            for name,schema in schemas.items():
                definitions={'s':'VARCHAR(%d)'%schema['width'],'v':'VECTOR(%d)'%schema['dim'],'pk':'BIGINT PRIMARY KEY','g':'VARCHAR(8)','n':'BIGINT'}
                sql('CREATE TABLE '+name+'('+','.join(col+' '+definitions[col] for col in schema['columns'])+')')
            sql('BEGIN');active=True
            for name in schemas:
                for first in range(0,320 if args.wide_spill else 800,80):
                    batch=[make_row(name,i-400) for i in range(first,first+80)]
                    sql('INSERT INTO '+name+' VALUES '+','.join(row_values(name,row) for row in batch),affected_rows=len(batch))
                    model[name].update({row['pk']:row for row in batch})
            if args.wide_spill:
                assert client.spool_exists(),'wide model did not exercise spill'
                report['initial_payload_spilled']=True
            snapshot();sql('COMMIT');active=False;committed=copy.deepcopy(model)
            for name,schema in schemas.items():
                if schema['dim']:sql('CREATE VECTOR INDEX vi_'+name+' ON '+name+'(v)')
            for step in range(args.steps):
                name=rng.choice(list(schemas));schema=schemas[name];op=rng.randrange(100)
                if op<9:
                    if not active:sql('BEGIN');active=True;operations['begin']+=1
                    else:
                        if rng.randrange(2):sql('COMMIT');committed=copy.deepcopy(model);operations['commit']+=1
                        else:sql('ROLLBACK');model=copy.deepcopy(committed);operations['rollback']+=1
                        active=False;snapshot()
                elif op<13:
                    crash=bool(rng.randrange(2));operations['kill' if crash else 'shutdown']+=1
                    if crash:client.kill()
                    else:client.shutdown()
                    client.start();active=False;model=copy.deepcopy(committed);snapshot()
                elif op<40:
                    available=[i for i in range(-500,1500) if i not in model[name]]
                    chosen=rng.sample(available,min(len(available),rng.randrange(1,5)))
                    if chosen:
                        batch=[make_row(name,i) for i in chosen]
                        sql('INSERT INTO '+name+' VALUES '+','.join(row_values(name,row) for row in batch),affected_rows=len(batch))
                        model[name].update({row['pk']:row for row in batch});commit_model();operations['insert']+=1
                elif op<60:
                    where,predicate=where_for(name);selected=[ident for ident,row in model[name].items() if predicate(row)]
                    value=make_row(name,0);columns=rng.sample([col for col in schema['columns'] if col!='pk'],rng.randrange(1,min(3,len(schema['columns']))))
                    sets=','.join(col+'='+(vliteral(value[col]) if col=='v' else literal(value[col])) for col in columns)
                    sql('UPDATE '+name+' SET '+sets+where,affected_rows=len(selected))
                    for ident in selected:
                        row=model[name][ident].copy();row.update({col:value[col] for col in columns});model[name][ident]=row
                    commit_model();operations['update']+=1
                elif op<70:
                    where,predicate=where_for(name);selected=[ident for ident,row in model[name].items() if predicate(row)]
                    sql('DELETE FROM '+name+where,affected_rows=len(selected))
                    for ident in selected:del model[name][ident]
                    commit_model();operations['delete']+=1
                elif op<81:
                    if model[name]:
                        new=next(i for i in range(2000,10000) if i not in model[name]);duplicate=rng.choice(list(model[name]))
                        if args.wide_spill and schema['dim']:
                            count=(1<<20)//(schema['width']*4)+8
                            identities=[i for i in range(2000,10000) if i not in model[name]][:count]
                            batch=[make_row(name,i) for i in identities]
                            for row in batch:row['s']='staged-'+str(row['pk'])
                            sql('INSERT INTO '+name+' VALUES '+','.join(row_values(name,row) for row in batch)+','+row_values(name,make_row(name,duplicate)),expected_error=True)
                            snapshot()
                            sql('INSERT INTO '+name+' VALUES '+row_values(name,batch[0]),affected_rows=1)
                            model[name][batch[0]['pk']]=batch[0];commit_model();operations['failed_spill_then_offset_reuse']+=1
                        else:
                            sql('INSERT INTO '+name+' VALUES '+row_values(name,make_row(name,new))+','+row_values(name,make_row(name,duplicate)),expected_error=True)
                    else:sql('UPDATE '+name+' SET s='+literal('x'*(schema['width']+1)),expected_error=True)
                    operations['failed_statement']+=1;snapshot()
                else:query(name);operations['query']+=1
                query(name)
                if step%40==0:snapshot()
            snapshot();client.kill();client.start();active=False;model=copy.deepcopy(committed);snapshot()
            means={str(key):sum(values)/len(values) for key,values in recalls.items() if values}
            assert all(value>=.95 for value in means.values()),means
            if args.wide_spill:assert operations['failed_spill_then_offset_reuse']>0
            report.update(passed=True,operation_counts=dict(operations),mean_recall=means,
                pk_not_first=True,multi_table_transaction_state=True,unicode_and_embedded_controls=True,
                failed_statement_atomicity=True,ordinary_and_vector_results_checked=True)
        finally:client.close()
except Exception as error:
    report.update(error_type=type(error).__name__,last_failure=last)
    raise
finally:
    target.write_text(json.dumps(report,ensure_ascii=False,indent=2)+'\n',encoding='utf-8')
    print(json.dumps({key:value for key,value in report.items() if key!='last_failure'},ensure_ascii=False),flush=True)
