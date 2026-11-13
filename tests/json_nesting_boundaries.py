"""Exercise the actual JSONL process without a recursive test-side decoder."""
import argparse,json,os,random,subprocess,tempfile
from pathlib import Path
ROOT=Path(__file__).resolve().parent.parent
p=argparse.ArgumentParser();p.add_argument('--binary',type=Path);p.add_argument('--image');p.add_argument('--output',type=Path);a=p.parse_args()
binary=a.binary or ROOT/('minivectordb.exe' if os.name=='nt' else 'minivectordb')
rng=random.Random(20261125);requests=0
def value(depth=0):
    if depth>=5 or rng.random()<0.4:return rng.choice([None,True,False,rng.randrange(-10**18,10**18),'中文\x00\n"\\'])
    if rng.random()<0.5:return [value(depth+1) for _ in range(rng.randrange(4))]
    return {str(i):value(depth+1) for i in range(rng.randrange(4))}
with tempfile.TemporaryDirectory(prefix='mvd-json-depth-') as directory:
    command=['docker','run','--rm','-i','--cpus=2','--memory=256m','-v',directory+':/data',a.image] if a.image else [str(binary),'--data-dir',directory]
    proc=subprocess.Popen(command,stdin=subprocess.PIPE,stdout=subprocess.PIPE,stderr=subprocess.PIPE,text=True,encoding='utf-8')
    try:
        assert proc.stdout.readline().rstrip('\n')=='{"type":"ready"}'
        def request(ident,sql,extra='',error=False):
            global requests
            proc.stdin.write('{"id":'+ident+',"op":"execute","sql":'+json.dumps(sql)+extra+'}\n');proc.stdin.flush()
            line=proc.stdout.readline().rstrip('\n');requests+=1
            assert line,(requests,proc.poll())
            if error:
                response=json.loads(line);assert response['ok'] is False and response['id'] is None
            else:assert line.startswith('{"id":'+ident+',"ok":true'),(requests,line[:200])
            return line
        request('0','CREATE TABLE t(id BIGINT PRIMARY KEY)')
        for _ in range(800):
            request(json.dumps(value(),ensure_ascii=False),'SELECT id FROM t',',"extension":'+json.dumps(value(),ensure_ascii=False))
        for depth in [1,128,1000,10000,50000,100000]:
            for left,right in [('[',']'),('{"x":','}'),('[{"x":','}]')]:
                ident=left*depth+'0'+right*depth
                request(ident,'SELECT id FROM t')
                request('1','SELECT id FROM t',',"extension":'+ident)
        for ident in ['9'*10000,'"'+'a'*100001+'"','1e999999999999999999999']:
            request(ident,'SELECT id FROM t')
        malformed=['[1,]','{"x":}','{"x" 1}','[1 2]','[true false]','[01]','[+1]','[-]','[.1]','[1.]','[1e]','{"x":1,}','[}', '{]']
        for depth in [0,128,50000]:
            for item in malformed:
                request('['*depth+item+']'*depth,'INSERT INTO t VALUES(99)',error=True)
        assert json.loads(request('2','SELECT id FROM t'))['rows']==[]
        proc.stdin.write('{"id":3,"op":"shutdown"}\n');proc.stdin.flush()
        assert json.loads(proc.stdout.readline())==dict(id=3,ok=True)
        assert proc.wait(timeout=10)==0
        report=dict(passed=True,requests=requests,max_json_containers=200000,random_json_ids=800,malformed_requests=42,malformed_sql_not_executed=True,exact_raw_id_echo=True)
    finally:
        if proc.poll() is None:proc.kill();proc.wait()
        proc.stdin.close();proc.stdout.close();proc.stderr.close()
if a.output:
    a.output.parent.mkdir(parents=True,exist_ok=True);a.output.write_text(json.dumps(report,indent=2)+'\n',encoding='utf-8')
print(json.dumps(report),flush=True)
