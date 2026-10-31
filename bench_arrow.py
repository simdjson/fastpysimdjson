"""Time NDJSON to Arrow and back: fastsimdjson.loads_arrow (and load_arrow_file)
against pyarrow.json.read_json and polars.read_ndjson, then
fastsimdjson.dumpb_arrow against polars.DataFrame.write_ndjson and DuckDB
(COPY ... TO a file, on tmpfs if TMPDIR points to one). With THREADS threads
(default 1). Reports the best of several runs in GB/s of JSON.

    THREADS=8 python bench_arrow.py flat.ndjson:flat gharchive.ndjson:gh flat.ndjson:infer

The schemas: flat (9 scalar fields), gh (6 fields of GH Archive events: two
structs, the other fields skipped), infer (none given).
"""
import os
import sys
import tempfile
import time

THREADS = int(os.environ.get("THREADS", "1"))
os.environ["POLARS_MAX_THREADS"] = str(THREADS)  # before polars is imported

import pyarrow as pa
import pyarrow.json as pj

import fastsimdjson

try:
    import polars as pl
except ImportError:
    pl = None
try:
    import duckdb
except ImportError:
    duckdb = None

ROUNDS = int(os.environ.get("ROUNDS", "5"))
pa.set_cpu_count(THREADS)
pa.set_io_thread_count(THREADS)

SCHEMAS = {
    "flat": pa.schema([("id", pa.int64()), ("user", pa.int64()), ("price", pa.float64()),
                       ("lat", pa.float64()), ("lon", pa.float64()), ("name", pa.string()),
                       ("tag", pa.string()), ("active", pa.bool_()), ("count", pa.int64())]),
    "gh": pa.schema([("id", pa.string()), ("type", pa.string()),
                     ("actor", pa.struct([("id", pa.int64()), ("login", pa.string())])),
                     ("repo", pa.struct([("id", pa.int64()), ("name", pa.string())])),
                     ("public", pa.bool_()), ("created_at", pa.string())]),
    "infer": None,
}


def best(fn):
    t = float("inf")
    for _ in range(ROUNDS):
        t0 = time.perf_counter()
        fn()
        t = min(t, time.perf_counter() - t0)
    return t


def report(task, path, name, who, size, fn):
    try:
        t = best(fn)
        print(f"{task},{os.path.basename(path)},{name},{who},{THREADS},{t:.4f},"
              f"{size / t / 1e9:.3f}", flush=True)
    except Exception as e:
        print(f"{task},{os.path.basename(path)},{name},{who},{THREADS},failed,"
              f"{type(e).__name__}", flush=True)


print("task,file,schema,library,threads,seconds,GBps")
for arg in sys.argv[1:]:
    path, name = arg.rsplit(":", 1)
    schema = SCHEMAS[name]
    data = open(path, "rb").read()
    read_options = pj.ReadOptions(use_threads=THREADS > 1)
    parse_options = (pj.ParseOptions() if schema is None else
                     pj.ParseOptions(explicit_schema=schema, unexpected_field_behavior="ignore"))
    readers = {
        "fastsimdjson": lambda: pa.table(fastsimdjson.loads_arrow(data, schema, threads=THREADS)),
        "fastsimdjson (file)": lambda: pa.table(
            fastsimdjson.load_arrow_file(path, schema, threads=THREADS)),
        "pyarrow": lambda: pj.read_json(pa.BufferReader(data), read_options=read_options,
                                        parse_options=parse_options),
    }
    if pl is not None:
        if schema is None:
            readers["polars"] = lambda: pl.read_ndjson(data, infer_schema_length=None)
        else:
            polars_schema = pl.from_arrow(schema.empty_table()).schema
            readers["polars"] = lambda: pl.read_ndjson(data, schema=polars_schema)
    for who, fn in readers.items():
        report("read", path, name, who, len(data), fn)
    if schema is None:
        continue
    table = pa.table(fastsimdjson.loads_arrow(data, schema, threads=THREADS))
    size = len(fastsimdjson.dumpb_arrow(table))
    writers = {"fastsimdjson": lambda: fastsimdjson.dumpb_arrow(table, threads=THREADS)}
    if pl is not None:
        frame = pl.from_arrow(table)
        writers["polars"] = lambda: frame.write_ndjson()
    if duckdb is not None:
        connection = duckdb.connect()
        connection.execute(f"SET threads = {THREADS}")
        connection.register("t", table)
        target = os.path.join(tempfile.gettempdir(), "fastsimdjson_bench.json")
        writers["duckdb"] = lambda: connection.execute(f"COPY t TO '{target}' (FORMAT json)")
    for who, fn in writers.items():
        report("write", path, name, who, size, fn)
