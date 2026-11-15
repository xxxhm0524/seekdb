"""Real-process tests of IDs and responses close to the official byte bound."""
import argparse
import hashlib
import json
from pathlib import Path

from native_test_support import NativeSession, ROOT, save


def run(out, candidate, reference):
    out.mkdir(exist_ok=False)
    ours=NativeSession(out,'ours',candidate)
    ref=NativeSession(out,'reference',reference)
    results=[]
    try:
        for db in (ours,ref):
            db.execute('CREATE TABLE t(id BIGINT PRIMARY KEY);')
            db.execute('INSERT INTO t VALUES(1);')
        for size in (1,16776704,16776960,16777000,16777088,16777120):
            ident='a'*size
            value=dict(id=ident,op='execute',sql='DELETE FROM t WHERE id=9;')
            request_bytes=len((json.dumps(value,ensure_ascii=False)+'\n').encode())
            responses=[]
            for db in (ours,ref):
                db.send(value);reply=db.read(60)
                responses.append(dict(ok=reply.get('ok'),id_exact=type(reply.get('id')) is str and reply.get('id')==ident,
                                      affected_rows=reply.get('affected_rows'),error=reply.get('error')))
            a,b=responses
            results.append(dict(category='id_near_response_limit',id_bytes=size,request_bytes=request_bytes,
                                request_within_reference_limit=request_bytes<=16777216,ours=a,reference=b,
                                qualifying_failure=b['ok'] and b['id_exact'] and (not a['ok'] or not a['id_exact'] or a['affected_rows']!=b['affected_rows'])))
            for db in (ours,ref):assert db.execute('SELECT id FROM t;')['rows']==[[1]]
        report=dict(passed=not any(x['qualifying_failure'] for x in results),observations=len(results),
                    results=results,candidate_sha256=hashlib.sha256(candidate.read_bytes()).hexdigest(),
                    reference_sha256=hashlib.sha256(reference.read_bytes()).hexdigest(),
                    scope='Actual JSONL requests within the unchanged official response bound; IDs summarized by size, no formal score.')
        save(out/'report.json',report)
        print(json.dumps(report),flush=True)
        return report
    finally:
        for db in (ours,ref):
            try:db.shutdown()
            finally:db.close()


if __name__=='__main__':
    p=argparse.ArgumentParser();p.add_argument('--output',required=True)
    p.add_argument('--candidate',default=str(ROOT/'build/minivectordb'))
    p.add_argument('--reference',default=str(ROOT/'build/reference-a08'))
    args=p.parse_args();result=run(Path(args.output),Path(args.candidate),Path(args.reference))
    raise SystemExit(0 if result['passed'] else 1)
