#!/usr/bin/env python3
"""ORDER BY must use the same float64 Euclidean value returned by L2_DISTANCE."""
import argparse
import json
import math
from pathlib import Path
import struct
import sys
import tempfile

ROOT=Path(__file__).resolve().parent.parent
sys.path.insert(0,str(ROOT/'tests'))
from protocol_client import Client
parser=argparse.ArgumentParser()
parser.add_argument('--rows',type=int,default=16000)
parser.add_argument('--image')
parser.add_argument('--output',type=Path)
args=parser.parse_args()
report=dict(passed=False,queries=0,rows=args.rows,target=args.image or 'native',
    oracle='Independent float32 values -> math.fsum of squares -> float64 sqrt; Python sort by the returned SQL scalar and signed PK.',
    scope='Returned-distance ordering regression; not proof of the unpublished formal failure cause.')
def f32(value):return struct.unpack('f',struct.pack('f',value))[0]
def vector(value):return '['+','.join('%.9g'%x for x in value)+']'
try:
    with tempfile.TemporaryDirectory(prefix='mvd-returned-order-') as data:
        if args.image:
            sys.path.insert(0,str(ROOT/'protocol_check'))
            from _transport import Session
            config=json.loads((ROOT/'protocol_check/config.json').read_text())
            log=args.output.with_suffix('.stderr.log') if args.output else Path(data)/'docker.stderr.log'
            log.parent.mkdir(parents=True,exist_ok=True)
            class ImageClient:
                def __init__(self):
                    self.session=Session(config,log,image=args.image);self.active=False
                    try:self.start()
                    except Exception:self.session.close();raise
                def start(self):self.session.start();self.active=True
                def request(self,text,mode='exact'):return self.session.request(text,mode)
                def kill(self):self.session.crash();self.active=False
                def shutdown(self):
                    if self.active:self.session.shutdown();self.active=False
                def close(self):
                    try:self.shutdown()
                    finally:self.session.close()
            client=ImageClient()
        else:client=Client(datadir=data,extra=['--cache-mb=1','--tx-memory-mb=1','--sort-memory-mb=1','--ann-min-rows=512'])
        try:
            def sql(text,mode='exact',**expected):
                result=client.request(text,mode);report['queries']+=1
                assert result.get('ok') is True,(text[:150],result)
                for key,value in expected.items():assert result.get(key)==value,(text[:150],key,result,value)
                return result
            def create(dim,rows):
                sql('CREATE TABLE t(id BIGINT PRIMARY KEY,g BIGINT,v VECTOR(%d),body VARCHAR(72))'%dim)
                records=list(rows.values())
                for first in range(0,len(records),100):
                    batch=records[first:first+100]
                    sql('INSERT INTO t VALUES '+','.join("(%d,%d,'%s','%s')"%(r['id'],r['g'],vector(r['v']),r['body']) for r in batch),affected_rows=len(batch))
            def verify(dim,model):
                zero=vector([0]*dim)
                expression="L2_DISTANCE(v,'%s')"%zero
                base=sql('SELECT id,g,'+expression+' AS d FROM t ORDER BY id',columns=['id','g','d'])['rows']
                assert [row[0] for row in base]==sorted(model)
                distances={}
                for ident,group,distance in base:
                    expected=math.sqrt(math.fsum(float(x)*float(x) for x in model[ident]['v']))
                    assert distance==expected,(ident,distance,expected)
                    assert group==model[ident]['g']
                    distances[ident]=distance
                for where,predicate in [('',lambda row:True),(' WHERE g=1',lambda row:row['g']==1),
                    (" WHERE body='keep'",lambda row:row['body']=='keep'),(' WHERE g=99',lambda row:False)]:
                    eligible=[ident for ident in model if predicate(model[ident])]
                    asc=sorted(eligible,key=lambda i:(distances[i],i))
                    desc=sorted(eligible,key=lambda i:(-distances[i],i))
                    mixed=sorted(eligible,key=lambda i:(distances[i],-model[i]['g'],i))
                    variants=[('SELECT id,'+expression+' AS d FROM t'+where+' ORDER BY d LIMIT 1',['id','d'],asc[:1]),
                        ('SELECT id,'+expression+' AS d FROM t'+where+' ORDER BY d LIMIT 7 OFFSET 3',['id','d'],asc[3:10]),
                        ('SELECT id,'+expression+' AS d FROM t'+where+' ORDER BY d',['id','d'],asc),
                        ('SELECT id,'+expression+' AS d FROM t'+where+' ORDER BY d DESC LIMIT 9',['id','d'],desc[:9]),
                        ('SELECT id,g,'+expression+' AS d FROM t'+where+' ORDER BY d,g DESC LIMIT 13 OFFSET 7',['id','g','d'],mixed[7:20]),
                        ('SELECT id,'+expression+' AS norm FROM t'+where+' ORDER BY 2 LIMIT 11',['id','norm'],asc[:11]),
                        ('SELECT id FROM t'+where+' ORDER BY '+expression+' LIMIT 8',['id'],asc[:8])]
                    for text,columns,wanted in variants:
                        for mode in ('exact','ann'):
                            result=sql(text,mode,columns=columns)
                            actual=[row[0] for row in result['rows']]
                            assert actual==wanted,(text,mode,actual[:20],wanted[:20])
                            if columns[-1] in ('d','norm'):
                                assert [row[-1] for row in result['rows']]==[distances[i] for i in wanted]
            # The initial two rows are a standalone RED case on 387cda5a.
            for scale in [1.0,2.0**-100,2.0**-40,2.0**40,2.0**100]:
                model={1:dict(id=1,g=1,v=[f32(scale),f32(scale*2.0**-27)],body='keep'),
                    2:dict(id=2,g=1,v=[f32(scale),0.0],body='keep')}
                create(2,model);verify(2,model)
                sql('CREATE VECTOR INDEX vi ON t(v)');verify(2,model)
                sql('DROP TABLE t')
            for dim,count in [(2,args.rows),(64,2048)]:
                perturbations=[2.0**-27,0.0,1e-8,1.5e-8,2e-8,1e-7,2.0**-28]
                ids=list(range(-count//2,count-count//2))
                ids[0]=-(2**63);ids[-1]=2**63-1
                model={ident:dict(id=ident,g=n%3,v=[1.0,f32(perturbations[n%len(perturbations)])]+[0.0]*(dim-2),
                    body='keep' if n%2 else 'skip') for n,ident in enumerate(ids)}
                create(dim,model);verify(dim,model)
                sql('CREATE VECTOR INDEX vi ON t(v)');verify(dim,model)
                saved={ident:dict(row,v=list(row['v'])) for ident,row in model.items()}
                sql('BEGIN')
                chosen=ids[1]
                updated=[0.0]*dim
                sql("UPDATE t SET v='%s' WHERE id=%d"%(vector(updated),chosen),affected_rows=1)
                model[chosen]['v']=updated
                verify(dim,model)
                sql('ROLLBACK');model=saved;verify(dim,model)
                client.kill();client.start();verify(dim,model)
                sql('DROP TABLE t')
            report.update(passed=True,heap_and_generic_order=True,aliases_ordinals_and_filters=True,
                signed_pk_ties=True,index_and_transaction_visibility=True,restart=True,
                external_sort_budget_mib=1 if not args.image else 'runtime default')
        finally:client.close()
finally:
    if args.output:
        args.output.parent.mkdir(parents=True,exist_ok=True)
        args.output.write_text(json.dumps(report,indent=2)+'\n',encoding='utf-8')
    print(json.dumps(report),flush=True)
