#!/usr/bin/env python3
"""Quoted and bare finite vectors must agree across INSERT/UPDATE/query paths."""
import argparse
import json
from pathlib import Path
import struct
import sys
import tempfile

ROOT=Path(__file__).resolve().parent.parent
sys.path.insert(0,str(ROOT/'tests'))
from protocol_client import Client

parser=argparse.ArgumentParser()
parser.add_argument('--output',type=Path)
parser.add_argument('--image')
parser.add_argument('--protocol-root',type=Path,default=ROOT/'protocol_check')
args=parser.parse_args()
report=dict(passed=False,checks=0,scope='Local parser consistency regression; not proof of the unpublished formal failure cause.')
report['target']=args.image or 'native binary'
cases=[('[1,2]','[1,2]',(1,2)),('[-1,2]','[-1,2]',(-1,2)),
    ('[- 1,+ 2]','[-1,+2]',(-1,2)),('[-.5,+.25]','[-.5,+.25]',(-.5,.25)),
    ('[-1.25e-3,+2.5e+2]','[-1.25e-3,+2.5e+2]',(-1.25e-3,250)),
    ('[-/* sign */1,+/* sign */2]','[-1,+2]',(-1,2)),
    ('[-0,+0]','[-0,+0]',(-0.0,0.0)),
    ('[-3.4028234663852886e38,+1e-40]','[-3.4028234663852886e38,+1e-40]',(-3.4028234663852886e38,1e-40))]
invalid=['[1 2,3]','[1e- 2,3]','[1e -2,3]','[--1,2]','[+-1,2]',
    '[1,,2]','[1,2,]','[[1],2]','[NaN,0]','[Infinity,0]','[1e100,0]']
try:
    with tempfile.TemporaryDirectory(prefix='mvd-vector-syntax-') as data:
        if args.image:
            sys.path.insert(0,str(args.protocol_root.resolve()))
            from _transport import Session
            config=json.loads((args.protocol_root/'config.json').read_text(encoding='utf-8'))
            log=(args.output.with_suffix('.stderr.log') if args.output else Path(data)/'docker.stderr.log')
            log.parent.mkdir(parents=True,exist_ok=True)
            class ImageClient:
                def __init__(self):
                    self.session=Session(config,log,image=args.image);self.active=False
                    try:self.start()
                    except Exception:
                        self.session.close();raise
                def start(self):self.session.start();self.active=True
                def request(self,text,mode='exact'):return self.session.request(text,mode)
                def shutdown(self):
                    if self.active:self.session.shutdown();self.active=False
                def kill(self):self.session.crash();self.active=False
                def close(self):
                    try:self.shutdown()
                    finally:self.session.close()
            client=ImageClient()
            report['transport_sha256']=__import__('hashlib').sha256((args.protocol_root/'_transport.py').read_bytes()).hexdigest()
        else:client=Client(datadir=data,extra=['--cache-mb=1'])
        try:
            def sql(text,**wanted):
                response=client.request(text);report['checks']+=1
                assert response.get('ok') is True,(text,response)
                for key,value in wanted.items():
                    actual=response.get(key)
                    if key=='rows' and response.get('columns')==['v']:
                        # JSON uses a round-trippable float32 decimal, which
                        # need not equal the exact float64 decoding of its bits.
                        actual=[[list(struct.unpack('ff',struct.pack('ff',*row[0])))] for row in actual]
                    assert actual==value,(text,key,response,value)
                return response
            sql('CREATE TABLE t(v VECTOR(2),pk BIGINT PRIMARY KEY)')
            for indexed in (False,True):
                if indexed:sql('CREATE VECTOR INDEX vi ON t(v)')
                for number,(bare,quoted,values) in enumerate(cases):
                    first=number*2+1
                    expected=list(struct.unpack('ff',struct.pack('ff',*values)))
                    sql("INSERT INTO t VALUES('%s',%d),(%s,%d)"%(quoted,first,bare,first+1),affected_rows=2)
                    sql('SELECT v FROM t WHERE pk=%d'%first,rows=[[expected]])
                    sql('SELECT v FROM t WHERE pk=%d'%(first+1),rows=[[expected]])
                    for mode in ('exact','ann'):
                        text='SELECT pk,L2_DISTANCE(v,%s) AS d FROM t WHERE pk>=%d AND pk<=%d ORDER BY d LIMIT 2'%(bare,first,first+1)
                        response=client.request(text,mode);report['checks']+=1
                        assert response.get('ok') is True and response['rows']==[[first,0],[first+1,0]],(text,response)
                    sql('BEGIN')
                    sql('UPDATE t SET v=%s WHERE pk=%d'%(bare,first),affected_rows=1)
                    sql('SELECT v FROM t WHERE pk=%d'%first,rows=[[expected]])
                    for bad in invalid:
                        for text in ['INSERT INTO t VALUES([0,0],9000),(%s,9001)'%bad,
                            'UPDATE t SET v=%s'%bad,
                            'SELECT pk FROM t ORDER BY L2_DISTANCE(v,%s) LIMIT 0'%bad]:
                            response=client.request(text);report['checks']+=1
                            assert response.get('ok') is False and response.get('error',{}).get('code'),(text,response)
                        sql('SELECT pk FROM t WHERE pk>=9000',rows=[])
                        sql('SELECT v FROM t WHERE pk=%d'%first,rows=[[expected]])
                    sql('ROLLBACK')
                sql('BEGIN');sql('UPDATE t SET v=[-7,+8] WHERE pk=1',affected_rows=1);sql('COMMIT')
                client.kill();client.start()
                sql('SELECT v FROM t WHERE pk=1',rows=[[[-7,8]]])
                sql('DELETE FROM t',affected_rows=len(cases)*2)
            report.update(passed=True,insert_update_query=True,malformed_literals_still_rejected=True,
                failed_statement_atomicity=True,index_and_restart=True)
        finally:client.close()
finally:
    if args.output:
        args.output.parent.mkdir(parents=True,exist_ok=True)
        args.output.write_text(json.dumps(report,indent=2)+'\n',encoding='utf-8')
    print(json.dumps(report),flush=True)
