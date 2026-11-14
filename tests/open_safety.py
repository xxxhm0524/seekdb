"""Unknown data must survive failed open; incomplete first init is retryable."""
import argparse
import hashlib
import json
from pathlib import Path
import subprocess
import sys
from client import BINARY, Client

def run(output):
    rejected=[]
    for name,payload in [("short",b"existing user data"),("large",b"UNKNOWN_DATABASE"+b"x"*20000),
                         ("other_version",b"MVDCP099"+b"\0"*9000)]:
        directory=output/name;directory.mkdir()
        path=directory/"database.mvd";path.write_bytes(payload)
        response=subprocess.run([BINARY,"--data-dir",str(directory)],input=b"",capture_output=True,timeout=10)
        assert response.returncode!=0 and b'"ready"' not in response.stdout,"unknown file was opened as an empty database"
        assert path.read_bytes()==payload,"opening an unknown database modified its bytes"
        rejected.append(dict(case=name,original_sha256=hashlib.sha256(payload).hexdigest(),preserved=True))
    seed=Client(output/"empty-seed");seed.kill()
    complete=(output/"empty-seed/database.mvd").read_bytes()
    assert len(complete)==8194
    # Derive partial-header fixtures from a real initialized file rather than
    # duplicating the implementation's header checksum calculation.
    cases=[b"",b"\0"*8192,b"\0"*8192+b"[",b"\0"*8192+b"[]"]
    for size in [1,3,7,8,16,40,100,4095,4096]:
        cases.append(b"\0"*4096+complete[4096:4096+size]+b"\0"*(4096-size)+b"[]")
    for i,payload in enumerate(cases):
        directory=output/f"initial-{i}";directory.mkdir()
        (directory/"database.mvd").write_bytes(payload)
        db=Client(directory)
        try:
            db.execute("CREATE TABLE t(id BIGINT PRIMARY KEY);")
            db.execute("INSERT INTO t VALUES(7);")
            db.restart(crash=True)
            assert db.execute("SELECT id FROM t;")["rows"]==[[7]]
        finally:db.kill()
    return dict(passed=True,unknown_files_rejected=rejected,incomplete_initialization_variants=len(cases))

if __name__=="__main__":
    sys.stdout.reconfigure(encoding="utf-8")
    p=argparse.ArgumentParser();p.add_argument("--output",required=True);args=p.parse_args()
    out=Path(args.output);out.mkdir(parents=True,exist_ok=False)
    try:result=run(out)
    except Exception as error:result=dict(passed=False,error=str(error))
    (out/"report.json").write_text(json.dumps(result,indent=2),encoding="utf-8")
    print(json.dumps(result),flush=True)
    sys.exit(0 if result["passed"] else 1)
