"""Exercise a file larger than the database container's memory limit."""
import json
from pathlib import Path
import tempfile

from test_database import Client


def main():
    dimension = 2048
    rows = 12000
    vector = "[1," + "0," * (dimension - 2) + "0]"
    with tempfile.TemporaryDirectory() as directory:
        client = Client(directory)
        try:
            client.sql(f"CREATE TABLE t(id BIGINT PRIMARY KEY,g BIGINT,v VECTOR({dimension}));")
            for start in range(0, rows, 100):
                batch = ",".join(f"({i},{i % 2},{vector})" for i in range(start, start + 100))
                assert client.sql(f"INSERT INTO t VALUES {batch};") == 100
            client.sql("CREATE VECTOR INDEX vi ON t(v);")
            query = f"SELECT id FROM t WHERE g=1 ORDER BY L2_DISTANCE(v,{vector}) LIMIT 3;"
            assert client.sql(query)[1] == [["1"], ["3"], ["5"]]
            assert client.sql("BEGIN;") == 0
            assert client.sql("DELETE FROM t WHERE id=1;") == 1
            assert client.sql(query)[1] == [["3"], ["5"], ["7"]]
        finally:
            client.close(kill=True)
        client = Client(directory)
        try:
            assert client.sql(query)[1] == [["1"], ["3"], ["5"]]
            assert len(client.sql("SELECT id FROM t;")[1]) == rows
            bytes_on_disk = (Path(directory) / "records.bin").stat().st_size
        finally:
            client.close()
    print(json.dumps({"rows": rows, "dimensions": dimension, "data_bytes": bytes_on_disk,
                      "committed_rows_recovered": rows, "filtered_topk": [1, 3, 5]}))


if __name__ == "__main__":
    main()
