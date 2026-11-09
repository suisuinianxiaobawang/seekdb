import math
import os
from pathlib import Path
import random
import struct
import subprocess
import uuid
import tempfile
import unittest

BIN = Path(os.environ.get("MINIVECTORDB_BIN", Path(__file__).resolve().parents[1] / "build/minivectordb"))
DDL = "CREATE TABLE docs (id BIGINT PRIMARY KEY, g BIGINT, s VARCHAR(10), v VECTOR(2));"


class Client:
    def __init__(self, directory):
        image = os.environ.get("MINIVECTORDB_IMAGE")
        self.container = f"mvdb-test-{uuid.uuid4().hex[:12]}" if image else None
        self.docker = ["sudo", "-n", "docker"] if os.environ.get("MINIVECTORDB_SUDO") else ["docker"]
        if image:
            memory = os.environ.get("MINIVECTORDB_MEMORY", "64m")
            command = self.docker + ["run", "--rm", "-i", "--name", self.container,
                                     "--platform", "linux/amd64", "--network", "none",
                                     "--cpus", "2", "--memory", memory, "--memory-swap", memory,
                                     "-v", f"{directory}:/data", image]
        else:
            command = [BIN, "--data-dir", directory]
        self.process = subprocess.Popen(
            command, stdin=subprocess.PIPE,
            stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True, encoding="utf-8",
        )
        try:
            assert self.line() == "READY"
        except BaseException:
            self.close(kill=True)
            raise

    def line(self):
        line = self.process.stdout.readline()
        if not line:
            raise RuntimeError("Database closed its output")
        return line.rstrip("\n")

    def sql(self, sql):
        self.process.stdin.write(sql + "\n")
        self.process.stdin.flush()
        header = self.line().split("\t")
        if header[0] == "RESULT":
            cols = self.line().split("\t")
            rows = [self.line().split("\t") for _ in range(int(header[1]))]
            assert len(cols) == int(header[2])
            assert all(len(r) == len(cols) for r in rows)
            return cols, rows
        if header[0] == "OK":
            return int(header[1])
        assert header[0] == "ERROR", header
        return header[1]

    def close(self, kill=False):
        if self.process.stdin.closed:
            return
        if kill and self.process.poll() is None:
            if self.container:
                subprocess.run(self.docker + ["kill", self.container], check=True,
                               stdout=subprocess.DEVNULL, timeout=10)
            else:
                self.process.kill()
        self.process.stdin.close()
        self.process.wait(timeout=10)
        error = self.process.stderr.read()
        self.process.stdout.close()
        self.process.stderr.close()
        if not kill:
            assert self.process.returncode == 0, error


class DatabaseTests(unittest.TestCase):
    def setUp(self):
        self.directory = tempfile.TemporaryDirectory()
        self.addCleanup(self.directory.cleanup)
        self.client = self.start()

    def start(self):
        client = Client(self.directory.name)
        self.addCleanup(client.close)
        return client

    def test_sql_types_and_unicode(self):
        sql = self.client.sql
        self.assertEqual(sql("CREATE TABLE t (s VARCHAR(2), id BIGINT, PRIMARY KEY(id));"), 0)
        self.assertEqual(sql("INSERT INTO t (id,s) VALUES (-9223372036854775808,'中🙂'),"
                             "(9223372036854775807,'a;'),(0,'é');"), 3)
        self.assertEqual(sql("SELECT id,s FROM t WHERE id >= -9223372036854775808 "
                             "AND s > 'a' ORDER BY s DESC LIMIT 3;")[1],
                         [["-9223372036854775808", "中🙂"], ["0", "é"], ["9223372036854775807", "a;"]])
        self.assertEqual(sql("INSERT INTO t VALUES ('🙂🙂🙂',9);"), "INVALID_VALUE")
        self.assertEqual(sql("INSERT INTO t VALUES ('x',9223372036854775808);"), "INVALID_VALUE")
        self.assertEqual(sql("UPDATE t SET id = 4;"), "UNSUPPORTED")
        self.assertEqual(sql("SELECT missing FROM t;"), "UNKNOWN_COLUMN")
        self.assertEqual(sql("SELECT id FROM t ORDER BY id LIMIT 0;")[1], [])
        self.assertEqual(sql("SELECT id FROM t WHERE id = 5;")[1], [])

    def test_primary_key_ranges_with_pending_rows(self):
        sql = self.client.sql
        sql(DDL)
        sql("INSERT INTO docs VALUES (-9223372036854775808,1,'a',[1,1]),"
            "(-1,1,'b',[1,1]),(0,1,'c',[1,1]),(2,1,'d',[1,1]),"
            "(9223372036854775807,1,'e',[1,1]);")
        self.assertEqual(sql("SELECT id FROM docs WHERE id < -9223372036854775808;")[1], [])
        self.assertEqual(sql("SELECT id FROM docs WHERE id > 9223372036854775807;")[1], [])
        self.assertEqual(sql("SELECT id FROM docs WHERE id=2 AND id<2;")[1], [])
        sql("BEGIN;")
        self.assertEqual(sql("DELETE FROM docs WHERE id>-2 AND id<1;"), 2)
        sql("INSERT INTO docs VALUES (1,2,'new',[0,0]),(-1,3,'back',[3,3]);")
        query = "SELECT id FROM docs WHERE id>=-1 AND id<=2 AND g>=1 ORDER BY id DESC LIMIT 2;"
        self.assertEqual(sql(query)[1], [["2"], ["1"]])
        self.assertEqual(sql("SELECT id FROM docs WHERE id>=-1 AND id<=2 ORDER BY id;")[1],
                         [["-1"], ["1"], ["2"]])
        sql("ROLLBACK;")
        self.assertEqual(sql(query)[1], [["2"], ["0"]])

    def test_order_and_filtered_topk(self):
        sql = self.client.sql
        self.assertEqual(sql(DDL), 0)
        self.assertEqual(sql("INSERT INTO docs VALUES (7,0,'é',[0,0]), (4,1,'z',[3,4]),"
                             "(2,1,'z',[-3,-4]), (1,1,'a',[6,8]);"), 4)
        self.assertEqual(sql("SELECT id,L2_DISTANCE(v,'[0,0]') AS d FROM docs WHERE g=1 "
                             "ORDER BY d LIMIT 2;")[1], [["2", "5"], ["4", "5"]])
        self.assertEqual(sql("SELECT id FROM docs WHERE g=1 ORDER BY s DESC;")[1], [["2"], ["4"], ["1"]])
        self.assertEqual(sql("SELECT id FROM docs WHERE g=1 ORDER BY s DESC LIMIT 2;")[1], [["2"], ["4"]])
        self.assertEqual(sql("CREATE VECTOR INDEX vi ON docs(v);"), 0)
        self.assertEqual(sql(".mode ann"), 0)
        self.assertEqual(sql("UPDATE docs SET v=[0,0] WHERE id=4;"), 1)
        self.assertEqual(sql("DELETE FROM docs WHERE id=7;"), 1)
        self.assertEqual(sql("SELECT id FROM docs ORDER BY L2_DISTANCE(v,[0,0]) LIMIT 2;")[1],
                         [["4"], ["2"]])
        self.assertEqual(sql(".mode exact"), 0)

    def test_indexed_topk_preserves_prefix_ties_and_tail(self):
        sql = self.client.sql
        self.assertEqual(sql("CREATE TABLE t(id BIGINT PRIMARY KEY,g BIGINT,v VECTOR(129));"), 0)
        zeros = [0] * 129

        def literal(first, last):
            return "[" + ",".join(map(str, [first] + zeros[1:-1] + [last])) + "]"

        values = ",".join(f"({i},{group},{literal(first, last)})" for i, group, first, last in
                          [(5, 1, 2, 0), (2, 1, -2, 0), (1, 1, 2, 1),
                           (3, 1, 10, 0), (4, 0, 0, 0)])
        self.assertEqual(sql("INSERT INTO t VALUES" + values + ";"), 5)
        self.assertEqual(sql("CREATE VECTOR INDEX vi ON t(v);"), 0)
        target = literal(0, 0)
        self.assertEqual(sql(f"SELECT id FROM t WHERE g=1 ORDER BY L2_DISTANCE(v,{target}) LIMIT 1;")[1],
                         [["2"]])
        self.assertEqual(sql(f"SELECT id FROM t WHERE g=1 ORDER BY L2_DISTANCE(v,{target}) DESC LIMIT 1;")[1],
                         [["3"]])
        self.assertEqual(sql(f"SELECT id FROM t ORDER BY L2_DISTANCE(v,{target}) LIMIT 3;")[1],
                         [["4"], ["2"], ["5"]])

    def test_float32(self):
        sql = self.client.sql
        sql(DDL)
        for literal in ("[NaN,0]", "[Infinity,0]", "[1e40,0]", "[1]", "[1,2,3]", "[1e,2]"):
            with self.subTest(literal=literal):
                self.assertEqual(sql(f"INSERT INTO docs VALUES(1,1,'x',{literal});"), "INVALID_VALUE")
        self.assertEqual(sql("INSERT INTO docs VALUES(1,1,'x',[16777217,1e-60]),"
                             "(2,1,'x',[-3e38,0]);"), 2)
        self.assertEqual(sql("SELECT L2_DISTANCE(v,[16777216,0]) FROM docs WHERE id=1;")[1], [["0"]])
        distance = float(sql("SELECT L2_DISTANCE(v,[3e38,0]) FROM docs WHERE id=2;")[1][0][0])
        self.assertTrue(math.isfinite(distance))
        self.assertGreater(distance, 5.9e38)

    def test_atomic_statements_and_rollback(self):
        sql = self.client.sql
        sql(DDL)
        sql("INSERT INTO docs VALUES (1,1,'x',[1,1]);")
        self.assertEqual(sql("BEGIN;"), 0)
        self.assertEqual(sql("UPDATE docs SET v=[0,0] WHERE id=1;"), 1)
        self.assertEqual(sql("INSERT INTO docs VALUES (2,1,'y',[2,2]),(1,1,'dup',[3,3]);"), "DUPLICATE_KEY")
        self.assertEqual(sql("INSERT INTO docs VALUES (3,1,'ok',[3,3]),(4,1,'bad',[4]);"), "INVALID_VALUE")
        self.assertEqual(sql("SELECT id,L2_DISTANCE(v,[0,0]) FROM docs ORDER BY id;")[1], [["1", "0"]])
        self.assertEqual(sql("COMMIT;"), 0)
        self.assertEqual(sql("BEGIN;"), 0)
        sql("CREATE TABLE ephemeral (id BIGINT PRIMARY KEY);")
        sql("INSERT INTO ephemeral VALUES(1);")
        sql("CREATE VECTOR INDEX vi ON docs(v);")
        sql("DELETE FROM docs;")
        self.assertEqual(sql("SELECT id FROM docs;")[1], [])
        self.assertEqual(sql("ROLLBACK;"), 0)
        self.assertEqual(sql("SELECT id FROM ephemeral;"), "UNKNOWN_TABLE")
        self.assertEqual(sql("CREATE VECTOR INDEX vi ON docs(v);"), 0)
        self.assertEqual(sql("SELECT id,L2_DISTANCE(v,[0,0]) FROM docs;")[1], [["1", "0"]])

    def test_crash_and_repeat_recovery(self):
        sql = self.client.sql
        sql(DDL)
        sql("BEGIN;")
        sql("INSERT INTO docs VALUES(10,1,'x',[1,1]),(20,1,'y',[2,2]);")
        sql("COMMIT;")
        sql("BEGIN;")
        sql("DELETE FROM docs WHERE id=10;")
        sql("UPDATE docs SET v=[9,9] WHERE id=20;")
        sql("INSERT INTO docs VALUES(30,1,'z',[3,3]);")
        self.client.close(kill=True)

        expected = [["10", "[1,1]"], ["20", "[2,2]"]]
        client = self.start()
        self.assertEqual(client.sql("SELECT id,v FROM docs ORDER BY id;")[1], expected)
        client.close(kill=True)
        # Simulate a process stopped while writing the next frame header.
        if not os.environ.get("MINIVECTORDB_IMAGE"):
            with open(Path(self.directory.name) / "records.bin", "ab") as data:
                data.write(struct.pack("=Q", 24) + b"\x00" * 11)
        client = self.start()
        self.assertEqual(client.sql("SELECT id,v FROM docs ORDER BY id;")[1], expected)
        client.sql("BEGIN;")
        client.sql("DELETE FROM docs;")
        client.close()
        self.assertEqual(self.start().sql("SELECT id,v FROM docs ORDER BY id;")[1], expected)

    def test_disk_flushed_statement_failure_preserves_prior_change(self):
        sql = self.client.sql
        sql("CREATE TABLE t(id BIGINT PRIMARY KEY,s VARCHAR(1400));")
        sql("INSERT INTO t VALUES(1,'old');")
        sql("BEGIN;")
        sql("UPDATE t SET s='kept' WHERE id=1;")
        payload = "x" * 1300
        rows = ",".join(f"({i},'{payload}')" for i in range(2, 262))
        self.assertEqual(sql(f"INSERT INTO t VALUES {rows},(1,'dup');"), "DUPLICATE_KEY")
        self.assertEqual(sql("SELECT id,s FROM t;")[1], [["1", "kept"]])
        sql("COMMIT;")
        self.client.close(kill=True)
        self.assertEqual(self.start().sql("SELECT id,s FROM t;")[1], [["1", "kept"]])

    def test_string_index_and_transaction_visibility(self):
        sql = self.client.sql
        sql(DDL)
        sql("INSERT INTO docs VALUES(1,0,'中',[1,0]),(2,0,'中',[2,0]),(3,0,'other',[0,0]);")
        sql("CREATE VECTOR INDEX vi ON docs(v);")
        query = "SELECT id FROM docs WHERE s='中' ORDER BY L2_DISTANCE(v,[0,0]) LIMIT 2;"
        self.assertEqual(sql(query)[1], [["1"], ["2"]])
        sql("BEGIN;")
        sql("DELETE FROM docs WHERE id=1;")
        sql("UPDATE docs SET s='中' WHERE id=3;")
        self.assertEqual(sql(query)[1], [["3"], ["2"]])
        sql("ROLLBACK;")
        self.assertEqual(sql(query)[1], [["1"], ["2"]])
        sql("UPDATE docs SET s='中' WHERE id=3;")
        self.assertEqual(sql(query)[1], [["3"], ["1"]])

    def test_multiline_protocol(self):
        sql = self.client.sql
        sql(DDL)
        self.assertEqual(sql("INSERT /* ; */ INTO docs VALUES\n-- ignored ;\n(1,1,'a'';b',[1,1]);"), 1)
        self.assertEqual(sql("SELECT s FROM docs;")[1], [["a';b"]])
        self.assertEqual(sql("SELECT id FROM docs WHERE g <> 1;"), "SYNTAX_ERROR")

    def test_randomized_topk_and_mutations(self):
        rng = random.Random(6709)
        sql = self.client.sql
        sql(DDL)
        sql("CREATE VECTOR INDEX vi ON docs(v);")
        rows = {i: (rng.randrange(5), rng.randrange(-20, 21), rng.randrange(-20, 21)) for i in range(250)}
        values = ",".join(f"({i},{g},'x',[{x},{y}])" for i, (g, x, y) in rows.items())
        self.assertEqual(sql("INSERT INTO docs VALUES" + values + ";"), len(rows))
        for step in range(80):
            group, a, b = rng.randrange(5), rng.randrange(-20, 21), rng.randrange(-20, 21)
            filtered = step % 2 == 0
            where = f"WHERE g={group}" if filtered else ""
            expected = sorted(((x-a)**2 + (y-b)**2, i) for i, (g, x, y) in rows.items()
                              if not filtered or g == group)[:10]
            actual = sql(f"SELECT id,L2_DISTANCE(v,[{a},{b}]) AS d FROM docs "
                         f"{where} ORDER BY d LIMIT 10;")[1]
            self.assertEqual([int(i) for i, _ in actual], [i for _, i in expected])
            for (_, d), (squared, _) in zip(actual, expected):
                self.assertAlmostEqual(float(d), math.sqrt(squared))
            key = rng.randrange(250)
            if key in rows:
                rows[key] = group, a, b
                self.assertEqual(sql(f"UPDATE docs SET g={group},v=[{a},{b}] WHERE id={key};"), 1)
                if step % 3 == 0:
                    del rows[key]
                    self.assertEqual(sql(f"DELETE FROM docs WHERE id={key};"), 1)

    def test_large_update_and_recovery(self):
        sql = self.client.sql
        sql("CREATE TABLE t(id BIGINT PRIMARY KEY, g BIGINT, v VECTOR(512));")
        vec = "[1," + "0," * 510 + "0]"
        for start in range(0, 4000, 100):
            rows = ",".join(f"({i},1,{vec})" for i in range(start, start + 100))
            self.assertEqual(sql("INSERT INTO t VALUES " + rows + ";"), 100)
        sql("BEGIN;")
        self.assertEqual(sql("UPDATE t SET g=2 WHERE g=1;"), 4000)
        self.assertEqual(sql("SELECT id FROM t WHERE g=2 ORDER BY id DESC LIMIT 2;")[1],
                         [["3999"], ["3998"]])
        sql("COMMIT;")
        self.client.close(kill=True)
        sql = self.start().sql
        self.assertEqual(sql(f"SELECT id FROM t WHERE g=2 ORDER BY L2_DISTANCE(v,{vec}) LIMIT 2;")[1],
                         [["0"], ["1"]])


if __name__ == "__main__":
    unittest.main()
