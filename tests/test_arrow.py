"""Tests of loads_arrow, load_arrow_file and dumpb_arrow, against pyarrow."""
import io
import json
import math
import mmap
import random

import pytest

import fastsimdjson

pa = pytest.importorskip("pyarrow")
import pyarrow.json as pj  # noqa: E402


def pyarrow_read(data, schema=None):
    options = (pj.ParseOptions() if schema is None else
               pj.ParseOptions(explicit_schema=schema, unexpected_field_behavior="ignore"))
    return pj.read_json(io.BytesIO(data), parse_options=options)


def ours(data, schema=None, **kw):
    return pa.table(fastsimdjson.loads_arrow(data, schema, **kw))


SCHEMA = pa.schema([("i", pa.int64()), ("u", pa.uint64()), ("d", pa.float64()),
                    ("s", pa.string()), ("b", pa.bool_()),
                    ("o", pa.struct([("x", pa.int64()), ("y", pa.list_(pa.string()))]))])


def random_rows(n, seed=1):
    rng = random.Random(seed)
    rows = []
    for _ in range(n):
        row = {
            "i": rng.choice([None, rng.randint(-2 ** 63, 2 ** 63 - 1)]),
            "u": rng.choice([None, rng.randint(0, 2 ** 64 - 1)]),
            "d": rng.choice([None, rng.uniform(-1e300, 1e300), rng.random(), float(rng.randint(-9, 9))]),
            "s": rng.choice([None, "".join(rng.choice('ab"\\\né\U0001f600') for _ in range(rng.randint(0, 9)))]),
            "b": rng.choice([None, True, False]),
            "o": rng.choice([None, {"x": rng.randint(0, 5), "y": [rng.choice(["p", None])] * rng.randint(0, 3)}]),
        }
        rows.append({k: v for k, v in row.items() if v is not None or rng.random() < 0.5})
    return rows


def ndjson(rows):
    return "".join(json.dumps(r) + "\n" for r in rows).encode()


def test_schema():
    data = ndjson(random_rows(2000))
    assert ours(data, SCHEMA).equals(pyarrow_read(data, SCHEMA))
    # a few records per batch
    t = ours(data, SCHEMA, block_size=1000)
    assert t.equals(pyarrow_read(data, SCHEMA)) and len(t.to_batches()) > 10


def test_infer():
    rows = [{"a": 1, "b": "x", "c": [1.5, None]}, {"a": None, "b": "y", "d": {"e": True}},
            {"a": 2.5, "c": [], "d": {"f": "g"}}, {}]
    data = ndjson(rows)
    t = ours(data)
    assert t.schema == pyarrow_read(data).schema
    assert t.equals(pyarrow_read(data, t.schema))


@pytest.mark.parametrize("threads", [None, 0, 2, 7])
def test_threads(threads):
    data = ndjson(random_rows(20000, seed=threads or 3))
    expected = ours(data, SCHEMA)
    assert ours(data, SCHEMA, block_size=4096, threads=threads).equals(expected)
    assert ours(data, block_size=4096, threads=threads).equals(ours(data))
    assert fastsimdjson.dumpb_arrow(expected, threads=threads) == fastsimdjson.dumpb_arrow(expected)


def test_inputs(tmp_path):
    data = ndjson(random_rows(300))
    expected = ours(data, SCHEMA)
    for x in [bytearray(data), memoryview(data), data.decode(), memoryview(b"xx" + data)[2:]]:
        assert ours(x, SCHEMA).equals(expected)
    # input that ends at the end of a page
    with mmap.mmap(-1, mmap.PAGESIZE * (len(data) // mmap.PAGESIZE + 2)) as m:
        m[-len(data):] = data
        m[:-len(data)] = b" " * (len(m) - len(data))
        assert ours(memoryview(m)[-len(data):], SCHEMA).equals(expected)
    path = tmp_path / "data.ndjson"
    path.write_bytes(data)
    for p in [path, str(path)]:
        assert pa.table(fastsimdjson.load_arrow_file(p, SCHEMA)).equals(expected)
    assert pa.table(fastsimdjson.load_arrow_file(path, threads=4)).equals(ours(data))
    with pytest.raises(FileNotFoundError):
        fastsimdjson.load_arrow_file(tmp_path / "missing.ndjson")
    (tmp_path / "empty.ndjson").write_bytes(b"")
    assert pa.table(fastsimdjson.load_arrow_file(tmp_path / "empty.ndjson", SCHEMA)).num_rows == 0
    (tmp_path / "bad.ndjson").write_bytes(b'{"i": "x"}\n')
    with pytest.raises(ValueError, match="row 0"):
        fastsimdjson.load_arrow_file(tmp_path / "bad.ndjson", SCHEMA)
    with pytest.raises(TypeError):
        fastsimdjson.loads_arrow(42)
    assert ours(b"", SCHEMA).num_rows == 0
    assert ours(b"").num_rows == 0


def test_stream_object():
    s = fastsimdjson.loads_arrow(b'{"a": 1}\n{"a": 2}\n')
    assert isinstance(s, fastsimdjson.ArrowStream)
    assert s.num_rows == 2
    assert pa.schema(s) == pa.schema([("a", pa.int64())])
    reader = pa.RecordBatchReader.from_stream(s)
    assert s.num_rows is None
    assert reader.read_all().column("a").to_pylist() == [1, 2]
    with pytest.raises(ValueError, match="already taken"):
        pa.table(s)
    with pytest.raises(ValueError, match="already taken"):
        fastsimdjson.dumpb_arrow(s)
    pa.schema(s)  # the schema stays
    fastsimdjson.loads_arrow(b'{"a": 1}\n')  # dropped without being read
    with pytest.raises(TypeError):
        fastsimdjson.ArrowStream()


def test_schema_argument():
    data = b'{"a": 1, "b": "x"}\n'
    capsule = pa.schema([("a", pa.int64())]).__arrow_c_schema__()
    assert ours(data, capsule).column_names == ["a"]
    assert ours(data, schema=pa.schema([("b", pa.string())])).column_names == ["b"]
    with pytest.raises(TypeError, match="__arrow_c_schema__"):
        fastsimdjson.loads_arrow(data, {"a": "int64"})
    with pytest.raises(ValueError, match="unsupported schema"):
        fastsimdjson.loads_arrow(data, pa.schema([("a", pa.timestamp("s"))]))


def test_errors():
    with pytest.raises(ValueError, match="row 1"):
        fastsimdjson.loads_arrow(b'{"a": 1}\n{"a": "x"}\n')
    with pytest.raises(ValueError):
        fastsimdjson.loads_arrow(b'{"a": 1}\n{"a": [}\n', pa.schema([("a", pa.int64())]))
    with pytest.raises(ValueError, match="b"):
        fastsimdjson.loads_arrow(b'{"a": 1, "b": 2}\n', pa.schema([("a", pa.int64())]),
                                 error_on_unknown_fields=True)
    with pytest.raises(ValueError):
        fastsimdjson.loads_arrow(b'{"a": 1}\n', block_size=0)
    with pytest.raises(ValueError):
        fastsimdjson.loads_arrow(b'{"a": 1}\n', threads=-1)
    # skipped values are validated, unless we ask not to
    bad = b'{"a": 1, "b": [tru]}\n'
    schema = pa.schema([("a", pa.int64())])
    with pytest.raises(ValueError):
        fastsimdjson.loads_arrow(bad, schema)
    assert ours(bad, schema, validate_skipped=False).column("a").to_pylist() == [1]


def test_dumpb():
    rows = random_rows(3000)
    table = pa.Table.from_pylist(rows, schema=SCHEMA)
    data = fastsimdjson.dumpb_arrow(table)
    expected = [{k: r.get(k) for k in SCHEMA.names} for r in table.to_pylist()]
    assert [json.loads(line) for line in data.splitlines()] == expected
    assert ours(data, SCHEMA).equals(table)
    compact = fastsimdjson.dumpb_arrow(table, write_nulls=False)
    assert ours(compact, SCHEMA).equals(table) and len(compact) < len(data)
    # a reader, a record batch, a struct array, our own stream
    assert fastsimdjson.dumpb_arrow(pa.RecordBatchReader.from_batches(
        table.schema, table.to_batches(max_chunksize=100))) == data
    batch = table.to_batches()[0]
    head = fastsimdjson.dumpb_arrow(batch)
    assert data.startswith(head) and head.count(b"\n") == batch.num_rows
    assert fastsimdjson.dumpb_arrow(pa.StructArray.from_arrays(
        batch.columns, names=batch.schema.names)) == head
    assert fastsimdjson.dumpb_arrow(fastsimdjson.loads_arrow(data, SCHEMA)) == data
    assert fastsimdjson.dumpb_arrow(table.slice(0, 0)) == b""


def test_dumpb_values():
    values = [0.1, -0.0, 1.0, 1e21, 1e-7, 5e-324, float("nan"), float("inf"), None, 2 ** 53 + 1.0]
    data = fastsimdjson.dumpb_arrow(pa.table({"d": pa.array(values, pa.float64())}))
    got = [json.loads(line)["d"] for line in data.splitlines()]
    assert got == [None if v is None or not math.isfinite(v) else v for v in values]
    assert data.splitlines()[0] == b'{"d":0.1}'
    table = pa.table({"s": ['a "b" \\ \n\x01é', None], "i8": pa.array([-128, 127], pa.int8()),
                      "v": pa.array(["x", "y" * 20], pa.string_view())})
    assert fastsimdjson.dumpb_arrow(table) == (
        b'{"s":"a \\"b\\" \\\\ \\n\\u0001\xc3\xa9","i8":-128,"v":"x"}\n'
        b'{"s":null,"i8":127,"v":"yyyyyyyyyyyyyyyyyyyy"}\n')


def test_dumpb_large():
    # the output grows many times, on one thread and on several
    n = 200000
    table = pa.table({"i": pa.array(range(n), pa.int64()),
                      "s": pa.array([str(i) * (i % 7) for i in range(n)])})
    expected = b"".join(b'{"i":%d,"s":"%s"}\n' % (i, (str(i) * (i % 7)).encode()) for i in range(n))
    for threads in [None, 1, 3]:
        assert fastsimdjson.dumpb_arrow(table, threads=threads) == expected


def test_dumpb_errors():
    with pytest.raises(TypeError, match="__arrow_c_stream__"):
        fastsimdjson.dumpb_arrow([{"a": 1}])
    with pytest.raises(ValueError, match="not supported|unsupported"):
        fastsimdjson.dumpb_arrow(pa.table({"t": pa.array([1], pa.timestamp("s"))}))
    with pytest.raises(TypeError):
        fastsimdjson.dumpb_arrow(pa.table({"a": [1]}), True)  # write_nulls is keyword-only


def test_polars():
    pl = pytest.importorskip("polars")
    data = ndjson(random_rows(500))
    frame = pl.DataFrame(fastsimdjson.loads_arrow(data, SCHEMA))
    assert frame.to_arrow().cast(SCHEMA).equals(ours(data, SCHEMA))
    out = fastsimdjson.dumpb_arrow(frame)
    assert ours(out, SCHEMA).equals(ours(data, SCHEMA))


def test_keyword_only():
    with pytest.raises(TypeError):
        fastsimdjson.loads_arrow(b"", None, 1024)  # block_size is keyword-only
