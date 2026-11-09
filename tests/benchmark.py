"""Deterministic local benchmark, with an independent scalar TopK check."""
import argparse
import json
import math
from pathlib import Path
import random
import statistics
import struct
import tempfile
import time

from test_database import Client


def float32(x):
    return struct.unpack("=f", struct.pack("=f", x))[0]


def literal(xs):
    return "[" + ",".join(map(str, xs)) + "]"


def run(n, d, q, check):
    rng = random.Random(748981)
    vectors = [[float32(rng.uniform(-1, 1)) for _ in range(d)] for _ in range(n)]
    queries = [[float32(rng.uniform(-1, 1)) for _ in range(d)] for _ in range(q)]
    inserts = ["INSERT INTO t VALUES " + ",".join(
        f"({i},{i % 10},{literal(vectors[i])})" for i in range(start, min(start + 100, n))) + ";"
        for start in range(0, n, 100)]
    selects = [f"SELECT id,L2_DISTANCE(v,{literal(v)}) AS d FROM t "
               + (f"WHERE g={i % 10} " if i % 2 else "") + "ORDER BY d LIMIT 10;"
               for i, v in enumerate(queries)]
    with tempfile.TemporaryDirectory() as directory:
        client = Client(directory)
        try:
            assert client.sql(f"CREATE TABLE t(id BIGINT PRIMARY KEY,g BIGINT,v VECTOR({d}));") == 0
            start = time.perf_counter()
            for i, s in enumerate(inserts):
                assert client.sql(s) == min(n - i * 100, 100)
            write = time.perf_counter() - start
            assert client.sql("CREATE VECTOR INDEX vi ON t(v);") == 0
            client.sql(selects[0])
            start = time.perf_counter()
            result = [client.sql(s)[1] for s in selects]
            query = time.perf_counter() - start
        finally:
            client.close()
        size = (Path(directory) / "records.bin").stat().st_size
    for i in range(min(q, check)):
        expected = sorted((sum((a-b)**2 for a, b in zip(vector, queries[i])), key)
                          for key, vector in enumerate(vectors) if not i % 2 or key % 10 == i % 10)[:10]
        assert [int(row[0]) for row in result[i]] == [key for _, key in expected]
        assert all(math.isclose(float(actual[1]), math.sqrt(squared), rel_tol=1e-12)
                   for actual, (squared, _) in zip(result[i], expected))
    return {"write_ms": round(write * 1000, 2), "query_ms": round(query * 1000, 2), "data_bytes": size}


def run_maintenance(n, d, q):
    """Time integer-vector import, narrow key ranges, and a wide-row update."""
    vector = literal([1] + [0] * (d - 1))
    text = "x" * 256
    with tempfile.TemporaryDirectory() as directory:
        client = Client(directory)
        try:
            assert client.sql(f"CREATE TABLE t(id BIGINT PRIMARY KEY,g BIGINT,s VARCHAR(256),v VECTOR({d}));") == 0
            batches = ["INSERT INTO t VALUES " + ",".join(
                f"({i},{i % 10},'{text}',{vector})" for i in range(start, min(start + 100, n))) + ";"
                for start in range(0, n, 100)]
            start = time.perf_counter()
            for i, sql in enumerate(batches):
                assert client.sql(sql) == min(n - i * 100, 100)
            write = time.perf_counter() - start
            requests = []
            for i in range(q):
                high = (i * 199) % n
                sql = f"SELECT id FROM t WHERE id>{high - 33} AND id<={high} ORDER BY id DESC LIMIT 10;"
                requests.append((sql, [[str(j)] for j in range(high, max(-1, high - 10), -1)]))
            start = time.perf_counter()
            for sql, expected in requests:
                assert client.sql(sql)[1] == expected
            key_range = time.perf_counter() - start
            start = time.perf_counter()
            assert client.sql("UPDATE t SET g=11;") == n
            update = time.perf_counter() - start
            assert client.sql("SELECT g,s,v FROM t WHERE id=0;")[1] == [["11", text, vector]]
        finally:
            client.close()
    return {"integer_write_ms": round(write * 1000, 2), "range_ms": round(key_range * 1000, 2),
            "update_ms": round(update * 1000, 2)}


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--rows", type=int, default=10000)
    parser.add_argument("--dimensions", type=int, default=128)
    parser.add_argument("--queries", type=int, default=1000)
    parser.add_argument("--check", type=int, default=8)
    parser.add_argument("--rounds", type=int, default=1)
    parser.add_argument("--maintenance", action="store_true")
    args = parser.parse_args()
    if args.maintenance:
        rounds = [run_maintenance(args.rows, args.dimensions, args.queries) for _ in range(args.rounds)]
    else:
        rounds = [run(args.rows, args.dimensions, args.queries, args.check) for _ in range(args.rounds)]
    print(json.dumps({"rounds": rounds, "median": {k: statistics.median(row[k] for row in rounds)
                                                  for k in rounds[0]}}, ensure_ascii=False))
