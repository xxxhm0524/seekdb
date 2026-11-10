#!/usr/bin/env python3
import sys, tempfile, os
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from run_tsv_public import C
def exp(c,sql,want):
    a=c.run(sql)
    print(('PASS' if a==want else 'FAIL'),sql,'=>',a,'expected',want)
    return a==want
def main():
    ok=True
    with tempfile.TemporaryDirectory(prefix='mvd-ext-') as d:
        c=C(d)
        try:
            ok &= exp(c,"CREATE TABLE t (id BIGINT, s VARCHAR(2), v VECTOR(2), PRIMARY KEY (id))",["OK"])
            ok &= exp(c,"INSERT INTO t VALUES (-9223372036854775808, 'a😀', '[1,2]'), (9223372036854775807, 'z', '[-1,2]')",["OK 2"])
            ok &= exp(c,"INSERT INTO t VALUES (3, 'abc', '[0,0]')",["ERROR varchar_too_long: s is too long"])
            ok &= exp(c,"INSERT INTO t VALUES (3, 'x', '[1]')",["ERROR dimension_mismatch: v must have 2 dimensions"])
            ok &= exp(c,"INSERT INTO t VALUES (3, 'x', '[NaN,0]')",["ERROR invalid_vector: expected numeric vector component"])
            ok &= exp(c,"INSERT INTO t VALUES (3, 'x', '[1e100,0]')",["ERROR vector_overflow: vector component overflows float32"])
            ok &= exp(c,"SELECT id FROM t ORDER BY s",["-9223372036854775808","9223372036854775807"])
            ok &= exp(c,"BEGIN",["OK"])
            ok &= exp(c,"UPDATE t SET s = 'q' WHERE id = -9223372036854775808",["OK 1"])
            ok &= exp(c,"INSERT INTO t VALUES (5, 'n', '[2,1]')",["OK 1"])
            ok &= exp(c,"SELECT id, s FROM t ORDER BY id",["-9223372036854775808\tq","5\tn","9223372036854775807\tz"])
            ok &= exp(c,"ROLLBACK",["OK"])
            ok &= exp(c,"SELECT id, s FROM t ORDER BY id",["-9223372036854775808\ta😀","9223372036854775807\tz"])
            ok &= exp(c,"BEGIN",["OK"])
            ok &= exp(c,"DELETE FROM t WHERE id = 9223372036854775807",["OK 1"])
            ok &= exp(c,"INSERT INTO t VALUES (7, 'c', '[0,0]')",["OK 1"])
            ok &= exp(c,"COMMIT",["OK"])
            ok &= exp(c,"SELECT id FROM t ORDER BY id",["-9223372036854775808","7"])
            ok &= exp(c,"BEGIN",["OK"])
            ok &= exp(c,"DELETE FROM t",["OK 2"])
            c.stop(); c.start()
            ok &= exp(c,"SELECT id, s FROM t ORDER BY id",["-9223372036854775808\ta😀","7\tc"])
            ok &= exp(c,"SELECT id, L2_DISTANCE(v, '[0,0]') AS d FROM t WHERE s = 'c'",["7\t0.000000"])
        finally: c.stop()
    print('ALL PASS' if ok else 'FAILURES')
    return 0 if ok else 1
if __name__=='__main__': sys.exit(main())
