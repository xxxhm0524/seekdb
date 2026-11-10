#!/usr/bin/env python3
"""Protocol-level regressions for boundaries absent from the public samples."""
import math
import os
import struct
import sys
import tempfile
import unittest

sys.path.insert(0, os.path.dirname(__file__))
from protocol_client import Client


class Boundaries(unittest.TestCase):
    def setUp(self):
        self.directory = tempfile.TemporaryDirectory(prefix="mvd-boundary-")
        self.client = Client(datadir=self.directory.name, extra=["--cache-mb=1"])

    def tearDown(self):
        self.client.close()
        self.directory.cleanup()

    def sql(self, text, **fields):
        response = self.client.request(text)
        self.assertIs(response.get("ok"), True, (text, response))
        for key, expected in fields.items():
            self.assertEqual(response.get(key), expected, (text, response))
        return response

    def rejected(self, text):
        response = self.client.request(text)
        self.assertIs(response.get("ok"), False, (text, response))
        self.assertTrue(response.get("error", {}).get("code"), response)

    def test_tiny_finite_distances(self):
        self.sql("CREATE TABLE t (id BIGINT PRIMARY KEY, v VECTOR(1));")
        self.sql("INSERT INTO t VALUES (1,'[2e-30]'),(2,'[1e-30]');")
        for suffix in ("LIMIT 2", "LIMIT 2 OFFSET 0", ""):
            result = self.sql("SELECT id,L2_DISTANCE(v,'[0]') AS d FROM t ORDER BY d " + suffix)
            self.assertEqual([r[0] for r in result["rows"]], [2, 1], result)
            self.assertGreater(result["rows"][0][1], 0, result)

    def test_distance_permutation_tie(self):
        self.sql("CREATE TABLE t (id BIGINT PRIMARY KEY, v VECTOR(4));")
        # The two float32 vectors have exactly equal squared norms. A float
        # accumulator loses the small terms in an order-dependent way.
        self.sql("INSERT INTO t VALUES (1,'[1,1,1,10000]'),(2,'[10000,1,1,1]');")
        for suffix in ("LIMIT 2", ""):
            result = self.sql("SELECT id,L2_DISTANCE(v,'[0,0,0,0]') AS d FROM t ORDER BY d " + suffix)
            self.assertEqual([r[0] for r in result["rows"]], [1, 2], result)

    def test_schema_integer_narrowing(self):
        for kind in ("VARCHAR", "VECTOR"):
            for length in (4294967297, 4294967298, 9223372036854775807):
                self.rejected("CREATE TABLE bad (id BIGINT PRIMARY KEY, x %s(%d));" % (kind, length))

    def test_order_validation_on_empty_results(self):
        self.sql("CREATE TABLE t (id BIGINT PRIMARY KEY, v VECTOR(2));")
        for suffix in ("", " LIMIT 0", " LIMIT 1"):
            for key in ("missing", "2", "L2_DISTANCE(v,'[0]') DESC"):
                self.rejected("SELECT id FROM t ORDER BY " + key + suffix)
        self.sql("INSERT INTO t VALUES (1,'[0,0]');")
        self.rejected("SELECT id FROM t WHERE id=99 ORDER BY missing;")
        self.rejected("SELECT id FROM t ORDER BY missing LIMIT 0;")

    def test_order_alias_matches_projected_column(self):
        self.sql("CREATE TABLE t (id BIGINT PRIMARY KEY, d BIGINT, v VECTOR(1));")
        self.sql("INSERT INTO t VALUES (1,20,'[0]'),(2,10,'[5]');")
        result = self.sql("SELECT id,d,L2_DISTANCE(v,'[0]') AS norm FROM t ORDER BY d LIMIT 2;")
        self.assertEqual([r[0] for r in result["rows"]], [2, 1], result)

    def test_invalid_vector_trailing_separator(self):
        self.sql("CREATE TABLE t (id BIGINT PRIMARY KEY,v VECTOR(2));")
        self.rejected("INSERT INTO t VALUES (1,'[1,2,]');")
        self.sql("SELECT id FROM t;", rows=[])

    def test_valid_subnormal_float32(self):
        self.sql("CREATE TABLE t (id BIGINT PRIMARY KEY,v VECTOR(1));")
        value = struct.unpack("f", struct.pack("f", 1e-40))[0]
        result = self.sql("INSERT INTO t VALUES (1,'[1e-40]');", affected_rows=1)
        result = self.sql("SELECT v,L2_DISTANCE(v,'[0]') AS d FROM t;")
        self.assertTrue(math.isclose(result["rows"][0][1], value, rel_tol=1e-8), result)

    def test_rollback_does_not_leak_heap_slots(self):
        self.sql("CREATE TABLE t (id BIGINT PRIMARY KEY);")
        self.sql("INSERT INTO t VALUES (1);")
        for _ in range(1100):
            self.sql("BEGIN;")
            self.sql("INSERT INTO t VALUES (2);")
            self.sql("ROLLBACK;")
        self.sql("INSERT INTO t VALUES (3);")
        self.client.shutdown()
        self.assertEqual(os.path.getsize(os.path.join(self.directory.name, 't_1.dat')), 65536)
        self.client = Client(datadir=self.directory.name)
        self.sql("SELECT id FROM t ORDER BY id", rows=[[1],[3]])

    def test_failed_update_preserves_prior_transaction(self):
        self.sql("CREATE TABLE t (id BIGINT PRIMARY KEY,s VARCHAR(2),v VECTOR(2));")
        self.sql("INSERT INTO t VALUES (1,'a','[0,0]'),(2,'b','[1,0]');")
        self.sql("BEGIN;")
        self.sql("UPDATE t SET s='ok' WHERE id=1;", affected_rows=1)
        self.rejected("UPDATE t SET s='too long',v='[9,9]' WHERE id<=2;")
        self.sql("SELECT id,s FROM t ORDER BY id", rows=[[1,'ok'],[2,'b']])
        self.sql("COMMIT;")
        self.client.shutdown()
        self.client = Client(datadir=self.directory.name)
        self.sql("SELECT id,s,v FROM t ORDER BY id", rows=[[1,'ok',[0,0]],[2,'b',[1,0]]])

    def test_patch_composition_reinsert_and_rollback(self):
        self.sql("CREATE TABLE t (id BIGINT PRIMARY KEY,s VARCHAR(4),v VECTOR(2));")
        self.sql("INSERT INTO t VALUES (1,'a','[0,0]'),(2,'b','[1,0]');")
        self.sql("BEGIN;")
        self.sql("UPDATE t SET s='x' WHERE id>=1", affected_rows=2)
        self.sql("UPDATE t SET v='[3,4]' WHERE s='x'", affected_rows=2)
        self.sql("INSERT INTO t VALUES (3,'n','[2,0]')", affected_rows=1)
        self.sql("UPDATE t SET s='new' WHERE id=3", affected_rows=1)
        self.sql("DELETE FROM t WHERE id=2", affected_rows=1)
        self.sql("INSERT INTO t VALUES (2,'r','[1,0]')", affected_rows=1)
        self.sql("UPDATE t SET s='q' WHERE id=2", affected_rows=1)
        self.sql("SELECT id,s,v FROM t ORDER BY id", rows=[[1,'x',[3,4]],[2,'q',[1,0]],[3,'new',[2,0]]])
        self.sql("SELECT id FROM t ORDER BY L2_DISTANCE(v,'[0,0]') LIMIT 3", rows=[[2],[3],[1]])
        self.sql("ROLLBACK;")
        self.sql("SELECT id,s,v FROM t ORDER BY id", rows=[[1,'a',[0,0]],[2,'b',[1,0]]])

    def test_vector_predicate_rejected_before_parallel_scan(self):
        self.sql("CREATE TABLE t (id BIGINT PRIMARY KEY,v VECTOR(2));")
        self.rejected("SELECT id FROM t WHERE v='[0,0]'")
        self.sql("INSERT INTO t VALUES (1,'[0,0]')", affected_rows=1)
        self.rejected("SELECT id FROM t WHERE v='[0,0]' ORDER BY L2_DISTANCE(v,'[0,0]') LIMIT 1")
        self.sql("SELECT id FROM t", rows=[[1]])


if __name__ == "__main__":
    unittest.main()
