"""Replay one SQL corpus against frozen/reference and refactored JSONL processes."""
import argparse,json,tempfile
from collections import Counter
from pathlib import Path
from protocol_client import Client
p=argparse.ArgumentParser();p.add_argument('--reference',type=Path,required=True);p.add_argument('--trace',type=Path,required=True);p.add_argument('--output',type=Path,required=True);a=p.parse_args()
def freeze(value):return tuple(freeze(x) for x in value) if isinstance(value,list) else value
count=0;restarts=0
with tempfile.TemporaryDirectory(prefix='mvd-refactor-diff-') as directory:
    first=Path(directory)/'reference';second=Path(directory)/'candidate';first.mkdir();second.mkdir()
    extra=['--cache-mb=1','--tx-memory-mb=1','--sort-memory-mb=1','--ann-min-rows=512']
    old=Client(binary=str(a.reference.resolve()),datadir=str(first),extra=extra);new=Client(datadir=str(second),extra=extra)
    try:
        for line in a.trace.read_text(encoding='utf-8').splitlines():
            item=json.loads(line);sql=item['sql'];mode=item.get('mode','exact')
            left=old.request(sql,mode);right=new.request(sql,mode);count+=1
            assert left['ok']==right['ok'],dict(sql=sql,left=left,right=right)
            if not left['ok']:
                assert left['error']['code']==right['error']['code'],dict(sql=sql,left=left,right=right)
            else:
                for key in ['columns','affected_rows']:assert left.get(key)==right.get(key),dict(sql=sql,left=left,right=right)
                if 'rows' in left:
                    if 'order by' in sql.lower():assert left['rows']==right['rows'],dict(sql=sql,left=left,right=right)
                    else:assert Counter(map(freeze,left['rows']))==Counter(map(freeze,right['rows'])),dict(sql=sql,left=left,right=right)
            if count%73==0:
                old.shutdown();new.shutdown();old.start();new.start();restarts+=1
            elif count%37==0:
                old.kill();new.kill();old.start();new.start();restarts+=1
    finally:old.close();new.close()
report=dict(passed=True,requests=count,restarts=restarts,reference=str(a.reference),scope='Frozen code differential, with extra normal and forced restarts; independent state/numeric oracles remain separate.')
a.output.parent.mkdir(parents=True,exist_ok=True);a.output.write_text(json.dumps(report,indent=2)+'\n',encoding='utf-8');print(json.dumps(report))
