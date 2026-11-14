#!/usr/bin/env python3
"""Verify multi-page rows through the normal JSONL entry point and recovery."""
import argparse
import json
from pathlib import Path
import sys
import tempfile

from protocol_client import Client


def verify(image):
    with tempfile.TemporaryDirectory(prefix="mvd-wide-") as data:
        if image:
            package = Path(__file__).resolve().parents[1] / "protocol_check"
            sys.path.insert(0, str(package))
            from _transport import Session

            config = json.loads((package / "config.json").read_text(encoding="utf-8"))
            client = Session(config, Path(data) / "docker.stderr.log", image=image)
            client.start()
            kill = client.crash
        else:
            client = Client(datadir=data, extra=["--cache-mb=1", "--tx-memory-mb=1"])
            kill = client.kill
        try:
            def sql(statement, mode="exact", **expected):
                response = client.request(statement, mode)
                assert response.get("ok") is True, (statement[:120], response)
                for key, value in expected.items():
                    assert response[key] == value, (statement[:120], key)

            sql("CREATE TABLE wide (id BIGINT PRIMARY KEY, s VARCHAR(25000), v VECTOR(2))")
            sql("CREATE TABLE narrow (id BIGINT PRIMARY KEY)")
            sql("CREATE TABLE maximum (id BIGINT PRIMARY KEY, s VARCHAR(65535))")
            sql("INSERT INTO maximum VALUES (1,'short value')")
            text = "😀" * 25000
            sql("BEGIN")
            for ident in range(1, 21):
                sql(f"INSERT INTO wide VALUES ({ident},'{text}',[{ident},0])", affected_rows=1)
            # Exceed even the default 8 MiB payload budget in the final image.
            for ident in range(100,180):
                sql(f"INSERT INTO wide VALUES ({ident},'{text}',[{ident},0])",affected_rows=1)
            if image:
                assert client.docker('exec',client.name,'test','-f','/data/txn.tmp',check=False).returncode==0
            else:
                assert client.spool_exists()
            sql("INSERT INTO narrow VALUES (1)")
            sql("COMMIT")
            sql("SELECT * FROM wide WHERE id=2", rows=[[2, text, [2, 0]]])
            sql("SELECT id FROM wide ORDER BY L2_DISTANCE(v,'[3.1,0]') LIMIT 1", rows=[[3]])
            sql("CREATE VECTOR INDEX vi ON wide(v)")
            sql("SELECT id FROM wide ORDER BY L2_DISTANCE(v,'[3.1,0]') LIMIT 1", "ann", rows=[[3]])
            sql("BEGIN")
            sql("UPDATE wide SET s='short',v=[0,0] WHERE id=2", affected_rows=1)
            sql("DELETE FROM wide WHERE id=3", affected_rows=1)
            sql("SELECT id FROM wide WHERE s='short' ORDER BY L2_DISTANCE(v,'[0,0]') LIMIT 1", rows=[[2]])
            sql("ROLLBACK")
            sql("SELECT * FROM wide WHERE id=2", rows=[[2, text, [2, 0]]])
            sql("BEGIN")
            sql("UPDATE wide SET s='committed' WHERE id=2", affected_rows=1)
            sql("DELETE FROM wide WHERE id=3", affected_rows=1)
            sql("INSERT INTO narrow VALUES (2)")
            sql("INSERT INTO wide VALUES (21,'deleted before commit',[0,0])")
            sql("DELETE FROM wide WHERE id=21")
            sql("INSERT INTO wide VALUES (22,'after the gap',[22,0])")
            sql("COMMIT")
            sql("BEGIN")
            sql("DELETE FROM wide WHERE id=2", affected_rows=1)
            sql("INSERT INTO wide VALUES (30,'uncommitted',[0,0])")
            kill()
            client.start()
            sql("SELECT * FROM wide WHERE id=2", rows=[[2, "committed", [2, 0]]])
            sql("SELECT id FROM wide WHERE id=3", rows=[])
            sql("SELECT id FROM wide WHERE id=30", rows=[])
            sql("SELECT id FROM wide WHERE id>=21 ORDER BY id", rows=[[22]]+[[i] for i in range(100,180)])
            sql("SELECT id FROM narrow ORDER BY id", rows=[[1], [2]])
            sql("SELECT s FROM maximum WHERE id=1", rows=[["short value"]])
            sql("INSERT INTO wide VALUES (3,'reinserted',[0,0])", affected_rows=1)
            client.shutdown()
            client.start()
            sql("SELECT id,s FROM wide WHERE id=3", rows=[[3, "reinserted"]])
            sql("SELECT id FROM wide ORDER BY L2_DISTANCE(v,'[0,0]') LIMIT 1", "ann", rows=[[3]])
        finally:
            client.close()
    print("Wide rows: PASS (spilled transaction, SQL and index, rollback, kill, restart)")


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("--image")
    verify(parser.parse_args().image)
