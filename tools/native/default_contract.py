import hashlib
import json
import os
from pathlib import Path
import sys
from native_test_support import NativeSession, ROOT, save

out=ROOT/'native-evidence'/sys.argv[1];out.mkdir(exist_ok=False)
db=NativeSession(out,'default-contract',Path(sys.argv[2]))
checks=0
try:
    proc=Path('/proc')/str(db.proc.pid)
    status=dict(line.split(':',1) for line in (proc/'status').read_text().splitlines() if ':' in line)
    environment=dict(uid=status['Uid'].split(),no_new_privileges=status['NoNewPrivs'].strip(),
                     root_flags=os.statvfs(proc/'root').f_flag,data_flags=os.statvfs(proc/'root/data').f_flag,
                     routes=(proc/'net/route').read_text().splitlines()[1:],cgroup=(proc/'cgroup').read_text())
    assert environment['uid']==['65534']*4 and environment['no_new_privileges']=='1'
    assert environment['root_flags']&os.ST_RDONLY and not environment['data_flags']&os.ST_RDONLY and not environment['routes']
    for table in ('key','order'):
        db.execute(f'CREATE TABLE {table}(id BIGINT PRIMARY KEY,n BIGINT,v VECTOR(2));')
        db.execute(f'INSERT INTO {table} VALUES(1,7,[1,0]);')
        db.execute(f'CREATE VECTOR INDEX same_name ON {table}(v);')
        checks+=3
    query='SELECT id,L2_DISTANCE(v,[0,0]) AS d FROM key ORDER BY d LIMIT 10;'
    for _ in range(4):assert db.execute(query)['rows']==[[1,1]];checks+=1
    db.execute('BEGIN;');db.execute('UPDATE key SET v=[0,0] WHERE id=1;')
    assert db.execute(query)['rows']==[[1,0]];checks+=1
    db.execute('ROLLBACK;');assert db.execute(query)['rows']==[[1,1]];checks+=1
    db.execute('BEGIN;');db.execute('INSERT INTO key VALUES(2,9,[0,0]);')
    assert not db.execute('INSERT INTO key VALUES(3,8,[0,1]),(1,9,[1,1]);',ok=False)['ok']
    assert db.execute('SELECT id FROM key ORDER BY id;')['rows']==[[1],[2]];checks+=2
    db.execute('COMMIT;');db.restart(crash=True)
    assert db.execute('SELECT id FROM key ORDER BY id;')['rows']==[[1],[2]];checks+=1
    for ending in ('shutdown','eof','sigkill'):
        db.execute('BEGIN;');db.execute('DELETE FROM key;')
        if ending=='shutdown':db.shutdown();db.start()
        elif ending=='sigkill':db.restart(crash=True)
        else:
            db.proc.stdin.close();assert db.proc.wait(timeout=60)==0
            assert db.proc.stdout.read()==b'';db.stop();db.start()
        assert db.execute('SELECT id FROM key ORDER BY id;')['rows']==[[1],[2]];checks+=1
    report=dict(passed=True,checks=checks,environment=environment,entrypoint_arguments=[],
                ready='Original official Session.start verified the ready object',shutdown_eof_and_sigkill_rollback=True,
                failed_statement_preserves_prior_transaction=True,same_index_name_on_distinct_tables=True,
                binary_sha256=hashlib.sha256(Path(sys.argv[2]).read_bytes()).hexdigest())
    save(out/'report.json',report);print(json.dumps(report),flush=True)
finally:
    try:db.shutdown()
    finally:db.close()
