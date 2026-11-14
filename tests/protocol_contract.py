"""Clause-driven JSONL checks using independent strict wire parsing."""
import argparse
import json
import math
from pathlib import Path
import queue
import sys
from client import Client, CONFIG


def strict(line):
    assert line.endswith(b'\n'),'response lacks a physical terminating LF'
    assert len(line)<=CONFIG['max_response_bytes'],'response exceeds the published byte limit'
    def pairs(items):
        out={}
        for key,value in items:
            assert key not in out,'duplicate response object key'
            out[key]=value
        return out
    value=json.loads(line.decode('utf-8','strict'),object_pairs_hook=pairs,
                     parse_constant=lambda x:(_ for _ in ()).throw(AssertionError('nonfinite response')))
    assert isinstance(value,dict),'response is not one JSON object'
    return value


class StrictClient(Client):
    def receive(self):
        line=self.lines.get(timeout=self.timeout)
        assert line is not None,'database exited before responding'
        self.last_bytes=len(line)
        return strict(line)


def run(out):
    db=StrictClient(out/'data',timeout=30);checks=[]
    def passed(name):checks.append(name)
    def idle():
        try:
            extra=db.lines.get(timeout=.05)
        except queue.Empty:return
        raise AssertionError('unsolicited or premature response: '+repr(extra)[:100])
    def raw(payload):
        db.process.stdin.write(payload);db.process.stdin.flush();return db.receive()
    try:
        # Client.start waits for the exact ready object before sending input.
        idle();passed('ready_before_input_exact_object_and_no_chatter')
        db.execute('CREATE TABLE t(id BIGINT PRIMARY KEY,body VARCHAR(128),v VECTOR(3));')
        text='你好😀; COMMIT; "quoted"\\slash\n\x00end'
        sql="INSERT INTO t VALUES(1,'"+text.replace('\\','\\\\').replace("'","''")+"','[0,1,0]');"
        request=dict(id='fragment',op='execute',sql=sql)
        encoded=json.dumps(request,ensure_ascii=False).encode('utf-8')
        split=encoded.index('😀'.encode())+2
        db.process.stdin.write(encoded[:split]);db.process.stdin.flush();idle()
        db.process.stdin.write(encoded[split:]);db.process.stdin.flush();idle()
        db.process.stdin.write(b'\n');db.process.stdin.flush();reply=db.receive()
        assert reply['id']=='fragment' and reply['ok'] is True and type(reply['affected_rows']) is int and reply['affected_rows']==1
        assert db.execute('SELECT body FROM t;')['rows']==[[text]]
        passed('fragmented_utf8_line_escaped_controls_semicolons_and_per_request_flush')
        small=db.execute("SELECT body,id,v,L2_DISTANCE(v,'[0,0,0]') AS d FROM t;")
        assert small['columns']==['body','id','v','d'] and small['rows'][0][:3]==[text,1,[0.0,1.0,0.0]]
        assert type(small['ok']) is bool and type(small['rows'][0][1]) is int
        assert type(small['rows'][0][3]) in (int,float) and math.isfinite(small['rows'][0][3])
        assert db.execute('SELECT id,body FROM t WHERE id=999;')['rows']==[]
        passed('query_columns_order_arrays_empty_rows_finite_distance_and_typed_bigint')
        for key in [-(2**63),2**53+1,2**63-1]:
            db.execute(f"INSERT INTO t VALUES({key},'bigint','[1,0,0]');")
            row=db.execute(f'SELECT id FROM t WHERE id={key};')['rows'][0][0]
            assert type(row) is int and row==key
        passed('signed_bigint_limits_and_beyond_double_precision')
        assert db.execute("UPDATE t SET body='updated' WHERE id=1;")['affected_rows']==1
        assert db.execute("UPDATE t SET body='none' WHERE id=999;")['affected_rows']==0
        assert db.execute('DELETE FROM t WHERE id=999;')['affected_rows']==0
        failed=db.execute("INSERT INTO t VALUES(99,'partial','[0,0,0]'),(1,'duplicate','[0,0,0]');",ok=False)
        assert type(failed['ok']) is bool and isinstance(failed['error']['code'],str) and failed['error']['code']
        assert db.execute('SELECT id FROM t WHERE id=99;')['rows']==[]
        passed('integer_affected_rows_nonfatal_sql_errors_and_statement_atomicity')
        valid_prefix=b'{"id":77,"op":"execute","sql":"DELETE FROM t;"'
        malformed=[valid_prefix+b'}trailing\n',valid_prefix+b'}{}\n',valid_prefix+b',"x":{"a":1,"a":2}}\n',
                   valid_prefix+b',"x":"\xff"}\n',valid_prefix+b'}\x00tail\n',valid_prefix+b',"x":NaN}\n',
                   valid_prefix+b',"x":Infinity}\n',valid_prefix+b',"x":"\\udc00"}\n',
                   b'DELETE FROM t;\n',b'\n']
        for payload in malformed:
            response=raw(payload)
            assert response['ok'] is False and response.get('error',{}).get('code')
            assert len(db.execute('SELECT id FROM t;')['rows'])==4
        passed('complete_json_utf8_no_duplicates_nonfinite_or_raw_sql_prefix_execution')
        for mode in ['exact','ann']:
            assert db.execute("SELECT id,L2_DISTANCE(v,'[0,0,0]') AS d FROM t ORDER BY d LIMIT 1;",mode)['rows']==[[-(2**63),1.0]]
        db.execute('SELECT id FROM t;','invalid',ok=False)
        passed('execute_search_modes_and_invalid_mode_rejection')
        db.execute('BEGIN;');db.execute("INSERT INTO t VALUES(88,'pending','[0,0,0]');")
        db.shutdown();passed('shutdown_ack_flush_exit_zero_and_no_extra_stdout')
        db.start();assert db.execute('SELECT id FROM t WHERE id=88;')['rows']==[]
        assert len(db.execute('SELECT id FROM t;')['rows'])==4
        passed('shutdown_rolls_back_uncommitted_and_preserves_confirmed_commits')
        db.execute('BEGIN;');db.execute("INSERT INTO t VALUES(89,'eof','[0,0,0]');")
        db.process.stdin.close();assert db.process.wait(timeout=30)==0
        db.reader.join(timeout=2);assert db.lines.get(timeout=2) is None
        db.start();assert db.execute('SELECT id FROM t WHERE id=89;')['rows']==[]
        passed('eof_rolls_back_and_emits_no_extra_response')
        idle();db.shutdown()
        other=StrictClient(out/'independent-case')
        try:other.execute('SELECT id FROM t;',ok=False);other.shutdown()
        finally:other.kill()
        passed('independent_case_directory_is_empty')
        return dict(passed=True,clause_scenarios=checks,malformed_frames_rejected=len(malformed),
                    independently_parsed_wire=True,max_response_bytes=CONFIG['max_response_bytes'])
    finally:db.kill()


if __name__=='__main__':
    sys.stdout.reconfigure(encoding='utf-8')
    p=argparse.ArgumentParser();p.add_argument('--output',required=True);args=p.parse_args()
    out=Path(args.output).resolve();out.mkdir(parents=True,exist_ok=False)
    try:result=run(out)
    except Exception as error:
        import traceback
        result=dict(passed=False,error=str(error),traceback=traceback.format_exc())
    (out/'report.json').write_text(json.dumps(result,ensure_ascii=False,indent=2),encoding='utf-8');print(json.dumps(result,ensure_ascii=False),flush=True)
    raise SystemExit(0 if result['passed'] else 1)
