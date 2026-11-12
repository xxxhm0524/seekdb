#!/usr/bin/env python3
"""A one-row result ordered by wide keys must use bounded working memory."""
import argparse
from contextlib import ExitStack
import json
from pathlib import Path
import sys
import tempfile

sys.path.insert(0,str(Path(__file__).resolve().parent))
from protocol_client import Client
from resource_boundaries import peak_bytes

parser=argparse.ArgumentParser()
parser.add_argument('--rows',type=int,default=12000)
parser.add_argument('--max-mb',type=int,default=64)
parser.add_argument('--cache-mb',type=int,default=1)
parser.add_argument('--sort-memory-mb',type=int,default=0)
parser.add_argument('--output',type=Path)
args=parser.parse_args()
assert args.rows>=1000
texts=[prefix+'\U0001f600'*2047 for prefix in ['b','a','中','\U0001f642']]
model={i:(i%7,texts[i%4],i) for i in range(args.rows)}
report=dict(passed=False,rows=args.rows,effective_key_bytes=sum(len(x[1].encode('utf-8')) for x in model.values()),
    page_cache_budget_bytes=args.cache_mb*1024*1024)
try:
    with tempfile.TemporaryDirectory(prefix='mvd-sort-keys-') as data, ExitStack() as cleanup:
        extra=['--cache-mb=%d'%args.cache_mb,'--tx-memory-mb=1']
        if args.sort_memory_mb:extra.append('--sort-memory-mb=%d'%args.sort_memory_mb)
        client=Client(datadir=data,extra=extra)
        cleanup.callback(client.close)
        def sql(text,**expected):
            value=client.request(text)
            assert value.get('ok') is True,(text[:120],value)
            for k,v in expected.items():assert value.get(k)==v,(k,text[:120],value)
            return value
        def batch(first,last):
            return ','.join("(%d,%d,'%s','[%d,0]')"%(i,i%7,texts[i%4],i) for i in range(first,last))
        def verify():
            ascending=sorted(model,key=lambda i:(model[i][1],-model[i][0],i))
            sql('SELECT id FROM t ORDER BY s ASC,g DESC,id ASC LIMIT 1 OFFSET %d'%(len(model)-1),
                rows=[[ascending[-1]]])
            sql('SELECT id,s AS text,g AS number FROM t ORDER BY 2 ASC,3 DESC,1 ASC LIMIT 3 OFFSET %d'%(len(model)//2),
                rows=[[i,model[i][1],model[i][0]] for i in ascending[len(model)//2:len(model)//2+3]])
            descending=sorted(model,key=lambda i:(model[i][1],-abs(model[i][2]),-i),reverse=True)
            chosen=descending[len(model)//2:len(model)//2+3]
            sql("SELECT id,L2_DISTANCE(v,'[0,0]') AS d FROM t ORDER BY s DESC,d ASC,id ASC LIMIT 3 OFFSET %d"%(len(model)//2),
                rows=[[i,abs(model[i][2])] for i in chosen])
            sql('SELECT id FROM t ORDER BY s ASC,g DESC,id ASC',rows=[[i] for i in ascending])
            repeated=','.join(['s ASC']*20+['s DESC','g DESC','id ASC'])
            sql('SELECT id FROM t ORDER BY '+repeated+' LIMIT 1 OFFSET %d'%(len(model)-1),rows=[[ascending[-1]]])
            numeric=sorted(model,key=lambda i:(-abs(model[i][2]),i))
            sql("SELECT id FROM t ORDER BY L2_DISTANCE(v,'[0,0]') DESC",rows=[[i] for i in numeric])
            sql("SELECT id FROM t ORDER BY L2_DISTANCE(v,'[0,0]') DESC,L2_DISTANCE(v,'[-0,0]') ASC,id ASC",
                rows=[[i] for i in numeric])
            invalid=client.request('SELECT id FROM t ORDER BY s,s,missing LIMIT 1 OFFSET 9223372036854775807')
            assert invalid.get('ok') is False,'normalization skipped ORDER BY validation'
            assert not (Path(data)/'sort.tmp').exists(),'query sort file leaked after response'
        sql('CREATE TABLE t(id BIGINT PRIMARY KEY,g BIGINT,s VARCHAR(2048),v VECTOR(2))')
        for first in range(0,args.rows,100):
            last=min(first+100,args.rows)
            sql('INSERT INTO t VALUES '+batch(first,last),affected_rows=last-first)
        client.shutdown();client.start()
        report['startup_peak_bytes']=peak_bytes(client.p.pid)
        ascending=sorted(model,key=lambda i:(model[i][1],-model[i][0],i))
        sql('SELECT id FROM t ORDER BY s ASC,g DESC,id ASC LIMIT 1 OFFSET %d'%(args.rows-1),rows=[[ascending[-1]]])
        peak=peak_bytes(client.p.pid)
        report['one_row_query_peak_bytes']=peak
        assert peak<=args.max_mb*1024*1024,('wide sort keys retained in memory',peak,args.max_mb)
        verify()
        sql('BEGIN')
        changed='zz'+'\U0001f600'*2046
        sql("UPDATE t SET s='%s',g=-9,v='[-1,0]' WHERE id=0"%changed,affected_rows=1)
        model[0]=(-9,changed,-1)
        sql("UPDATE t SET s='' WHERE id=2",affected_rows=1)
        model[2]=(2,'',2)
        for first in range(args.rows,args.rows+400,100):
            sql('INSERT INTO t VALUES '+batch(first,first+100),affected_rows=100)
            for i in range(first,first+100):model[i]=(i%7,texts[i%4],i)
        sql('DELETE FROM t WHERE id=4',affected_rows=1)
        del model[4]
        failed=client.request('INSERT INTO t VALUES '+batch(args.rows+500,args.rows+501)+",(0,0,'duplicate','[0,0]')")
        assert failed.get('ok') is False
        verify()
        sql('ROLLBACK')
        model={i:(i%7,texts[i%4],i) for i in range(args.rows)}
        verify()
        peak=peak_bytes(client.p.pid)
        report['peak_bytes']=peak
        assert peak<=args.max_mb*1024*1024,peak
        client.kill()
        (Path(data)/'sort.tmp').write_bytes(b'discarded query scratch')
        client.start()
        assert not (Path(data)/'sort.tmp').exists(),'stale query scratch not removed at startup'
        sql('SELECT g,s FROM t WHERE id=0',rows=[[0,texts[0]]])
        report.update(passed=True,scalar_distance_alias_ordinal_and_full_result=True,
            pending_spilled_rows_failed_statement_and_rollback=True,crash_scratch_cleanup=True)
finally:
    if args.output:
        args.output.parent.mkdir(parents=True,exist_ok=True)
        args.output.write_text(json.dumps(report,ensure_ascii=False,indent=2)+'\n',encoding='utf-8')
    print(json.dumps(report,ensure_ascii=False),flush=True)
