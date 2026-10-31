# fastsimdjson

A Python binding for [simdjson](https://github.com/simdjson/simdjson) that
parses JSON into native Python objects (`dict`, `list`, `str`, `int`,
`float`, `bool`, `None`) and serializes them back. It is a drop-in
replacement for the `json` module's `loads`, `load`, `dumps` and `dump`, and
it can be over 3 times faster than the standard `json.loads` and
`json.dumps`. When you only need part of a document, its lazy `parse`
function is faster still. It also reads streams of documents (NDJSON, JSON
Lines), on many threads with free-threaded Python, and can extract only the
fields you need from each document. It writes JSON in two ways: `dumps` returns the same `str` as
`json.dumps`, and `dumpb` returns the same `bytes` as `orjson.dumps`, as fast
as orjson; `dumpb_many` writes NDJSON, on many threads with free-threaded
Python. `loads_arrow` and `dumpb_arrow` convert NDJSON to Apache Arrow
tables and back, for pyarrow, polars or DuckDB, on many threads with any
Python. `json_to_msgpack` and `msgpack_to_json` convert JSON to
[MessagePack](https://msgpack.org/) and back without building Python objects,
3 to 4 times as fast as orjson with ormsgpack, on many threads with any
Python.

With pip:

```sh
pip install fastsimdjson
```

With uv:

```sh
uv pip install fastsimdjson      # in a uv project: uv add fastsimdjson
```

Wheels are available for Linux, macOS and Windows, for Python 3.10 to 3.15,
including free-threaded Python 3.14 and 3.15.

## Usage

### `loads`: the whole document

```python
import fastsimdjson
fastsimdjson.loads(b'{"a": [1, 2.5, "x", true, null]}')
# {'a': [1, 2.5, 'x', True, None]}
```

`loads(data)` accepts `bytes`, `bytearray`, `memoryview` and `str`, and
returns the same value as `json.loads`, with the same types and key order.
Integers that do not fit in 64 bits become exact Python ints. `NaN`,
`Infinity` and `-Infinity` are accepted, in any capitalization.

Invalid input raises `fastsimdjson.JSONDecodeError`, a subclass of
`json.JSONDecodeError`. When simdjson rejects a document, the same input is
parsed with `json.loads`. If that succeeds, `loads` returns its value (this
is how a number that overflows a double becomes `inf`). If it raises
`JSONDecodeError`, the exception is re-raised with Python's message and byte
position. Any other exception from `json.loads` propagates.

### `parse`: lazy views

When you need only part of a document, `parse` avoids building the rest.
It accepts the same inputs as `loads` and returns read-only views:
`fastsimdjson.Object` (a `Mapping`) and `fastsimdjson.Array` (a
`Sequence`). Values are converted when you access them; nested objects and
arrays are returned as views. A scalar root is returned as a plain value.

```python
doc = fastsimdjson.parse(open("twitter.json", "rb").read())
ids = [(s["id"], s["user"]["screen_name"]) for s in doc["statuses"]]
doc.at_pointer("/statuses/0/user/name")   # JSON Pointer (RFC 6901)
doc["search_metadata"].as_dict()          # convert a subtree, like loads
```

`Object` supports `obj[key]`, `get`, `in`, `len`, iteration over the keys,
`keys()`, `values()`, `items()` (iterators), `at_pointer` and `as_dict()`.
`Array` supports `arr[i]` (negative indexes and slices), `len`, iteration,
`at_pointer` and `as_list()`. Both work with `match` statements.

* A view keeps its document alive; the document owns its own buffers, so
  it remains valid while other documents are parsed.
* A key lookup scans the object. With duplicate keys, lookups return the
  first value, whereas `as_dict()` (like `json.loads`) keeps the last.
* Indexing an array walks it from the last index reached, so a loop over
  `arr[i]` is linear; iteration is the fastest way to visit an array.
* Errors are handled as in `loads`. A document that simdjson rejects but
  `json.loads` accepts (an overflowing number, an unpaired surrogate) is
  returned as plain Python objects, as `loads` would return it.

### `dumps` and `dump`: like `json.dumps`

```python
fastsimdjson.dumps({"a": [1, 2.5, None]})            # '{"a": [1, 2.5, null]}'
fastsimdjson.dumps(obj, indent=2, sort_keys=True)
with open("out.json", "w", encoding="utf-8") as f:
    fastsimdjson.dump(obj, f)
```

`dumps(obj, **kw)` takes the arguments of `json.dumps` and returns the same
`str`, character for character, including the float format (`repr`), the
escapes and the default separators. `ensure_ascii`, `indent`, `separators`,
`sort_keys`, `allow_nan` and `default` are handled in C. Everything else is
passed to `json.dumps` itself, which produces the result or raises its usual
exception: a `cls` argument or other encoder options, `skipkeys`, a circular
reference, `NaN` with `allow_nan=False`, a key or a value that `json` cannot
serialize. `dump(obj, fp, **kw)` writes `dumps(obj, **kw)` to `fp`.

### `dumpb`: like `orjson.dumps`

```python
fastsimdjson.dumpb({"a": [1, 2.5, None]})            # b'{"a":[1,2.5,null]}'
fastsimdjson.dumpb(obj, option=fastsimdjson.OPT_INDENT_2 | fastsimdjson.OPT_SORT_KEYS)
fastsimdjson.dumpb({1, 2}, default=sorted)           # b'[1,2]'
```

`dumpb(obj, default=None, option=None)` has the arguments, the output and the
errors of `orjson.dumps` (orjson 3.12): `orjson.dumps(obj, ...)` can be
replaced by `fastsimdjson.dumpb(obj, ...)` for the types below. It returns
compact UTF-8 `bytes`, writes floats as orjson does (`1e-6`, `1e+16`, `NaN`
and infinities as `null`), and raises `TypeError` with orjson's messages:
integers beyond 64 bits, a dict key that is not a `str`, invalid UTF-8 (a lone
surrogate), nesting deeper than 254, a type it cannot serialize. `orjson.JSONEncodeError` is a subclass of `TypeError`, so
`except TypeError` catches the errors of both.

It serializes `str`, `int`, `float`, `bool`, `None`, `dict`, `list`, `tuple`,
enums, and subclasses of `str`, `int`, `dict` and `list`. Anything else goes
to `default`, as in orjson: its result is serialized in place of the object,
and an exception it raises becomes the `__cause__` of the `TypeError`. The
options are exported under orjson's names and values (`OPT_APPEND_NEWLINE`,
`OPT_INDENT_2`, `OPT_NON_STR_KEYS`, `OPT_PASSTHROUGH_SUBCLASS`,
`OPT_SORT_KEYS`, `OPT_STRICT_INTEGER`, ...). Unlike orjson, `dumpb` does not
serialize dataclasses, `datetime`, `date`, `time`, `UUID`, numpy arrays or
`orjson.Fragment` itself: they go to `default`, as if
`OPT_PASSTHROUGH_DATACLASS` and `OPT_PASSTHROUGH_DATETIME` were set, and the
options that concern only these types are accepted and have no effect.

### `dumpb_many`: NDJSON, on several threads

```python
data = fastsimdjson.dumpb_many(records)               # b'{...}\n{...}\n'
data = fastsimdjson.dumpb_many(records, threads=16)   # free-threaded Python
```

`dumpb_many(records, default=None, option=None, *, threads=None)` returns
the records (a list, a tuple or any iterable) as NDJSON `bytes`: each record
as `dumpb` writes it, followed by a newline, so that the output reads back
with `loads_many(data, format="lines")`. `default` and `option` are those of
`dumpb`, except `OPT_INDENT_2`, which would spread a record over several
lines (`ValueError`); `OPT_APPEND_NEWLINE` changes nothing.

On free-threaded Python, `threads=N` writes chunks of records on N threads at
once (`threads=0` or `None`: one per processor), with the worker threads of
`map_many`. The output and the errors are those of one thread: when records
cannot be serialized, the error of the first of them is raised (`default` may
then have been called on later records, in other threads, and it must be
safe to call from several threads at once). With the GIL, the calling thread
does all the work. Like `dumpb`, `dumpb_many` does not crash when other
threads change the records meanwhile, but the output then reflects some of
the changes and not others.

### Files

```python
with open("data.json", "rb") as f:
    doc = fastsimdjson.load(f)          # like json.load: f.read(), then loads
doc = fastsimdjson.load_file("data.json")
view = fastsimdjson.parse_file("data.json")   # lazy, like parse
```

`load_file(path)` and `parse_file(path)` accept a `str`, `bytes` or
`os.PathLike` path and raise `OSError` (e.g. `FileNotFoundError`) when the
file cannot be read.

### `loads_many` and `parse_many`: streams of documents

```python
for record in fastsimdjson.loads_many(open("log.ndjson", "rb").read()):
    ...
for view in fastsimdjson.parse_many(data):    # lazy views, like parse
    ...
```

Both return an iterator over the documents of `data` (`bytes`, `bytearray`,
`memoryview` or `str`). The `format` keyword selects how documents are
separated:

| `format` | input |
|---|---|
| `"whitespace"` (default) | documents separated by white space, including NDJSON and JSON Lines |
| `"lines"` | one document per line (NDJSON, JSON Lines) |
| `"json_seq"` | RFC 7464 JSON text sequences (each document preceded by `\x1e`) |
| `"comma"` | documents separated by commas: `{...}, {...}` (simdjson also accepts white space between them) |
| `"array"` | the elements of one array: `[{...}, {...}]` |

simdjson parses the input in batches (`batch_size`, 1 MB by default); a
larger document is handled automatically. In every format, documents that
simdjson rejects are handled as in `loads`: a document that `json` accepts is
returned, otherwise `JSONDecodeError` reports `json`'s message and the
position in the whole input. A truncated last document is an error. The
views returned by `parse_many` remain valid after the iterator moves on.

### Streams on several threads: `threads=N` and `map_many`

On free-threaded Python (3.14t and newer), a stream can be parsed by several
threads at once:

```python
docs = fastsimdjson.loads_many(data, threads=16)        # a list, in order
views = fastsimdjson.parse_many(data, threads=16)       # a list of views
# fn runs in the threads too, on each document; its results, in order:
scores = fastsimdjson.map_many(score, data, threads=16)
```

With `threads`, `loads_many` and `parse_many` return a list of all the
documents instead of an iterator. `map_many(fn, data, **kw)` returns
`[fn(doc) for doc in loads_many(data, **kw)]` (`parse_many` with
`lazy=True`), with `fn` called in the threads. `threads=0` (or `None`) uses
one thread per processor; small inputs use fewer threads (about one per
512 KB). The input is cut into chunks at newlines (before each `\x1e` for
`json_seq`), so `threads` works with the formats `"whitespace"`, `"lines"`
and `"json_seq"`.

The results, and the exceptions, are those of one thread: when a chunk
contains an error or a document that only `json` accepts, the documents
before it are kept and the rest is parsed in the calling thread, as without
threads. An exception raised by `fn` is raised again, for the first document
where it happened; `fn` may then have been called on later documents, in
other threads, and it must be safe to call from several threads at once.

The worker threads are started by the first call that needs them and kept
for later calls (they wait, detached from the interpreter, between calls,
and stop at exit). `map_many` scales best: each document is built, used and
freed in the same thread, and only `fn`'s results come back. `loads_many` must return every
document, and the Python objects of all of them, to the calling thread.
With the GIL (the default build of Python), the calling thread does all the
work: `threads` has no effect on speed.

### Only some fields: `fields=`

When you need a few values from each document, `fields` extracts them
without building the documents: only the values found become Python
objects.

```python
events = fastsimdjson.loads_many(data, fields=["/type", "/payload/action"])
# (type, action) for each document

def count(t):
    kind, action, language = t
    ...
fastsimdjson.map_many(count, data, threads=64,
                      fields=["/type", "/payload/action",
                              "/payload/pull_request/base/repo/language"])
```

`loads_many` and `map_many` accept `fields`: a JSON Pointer (RFC 6901), or a
list or tuple of them. Each document then becomes the value at the pointer,
or a tuple of the values at the pointers, in order. A value that is an
object or an array is built in full. A pointer that leads nowhere (a
missing key, an index past the end, or a step into a scalar) gives
`default`, `None` unless you pass another value; a JSON `null` gives `None`.
As with `at_pointer`, a key that appears twice in an object refers to its
first occurrence. `fields` cannot be combined with `lazy=True`.

On 7.6 GB of GitHub events, `map_many` with three fields ran at 2.8 GB/s on
one core of an Intel Xeon Gold 6548N, against 2.2 GB/s with lazy views
(`lazy=True`). With 64 threads, it ran at 78 GB/s (lazy views: 61 GB/s).

### Arrow: `loads_arrow`, `load_arrow_file` and `dumpb_arrow`

```python
import pyarrow as pa
table = pa.table(fastsimdjson.loads_arrow(data))             # inferring the schema
schema = pa.schema([("id", pa.int64()), ("name", pa.string())])
table = pa.table(fastsimdjson.load_arrow_file("log.ndjson", schema, threads=8))
frame = polars.DataFrame(fastsimdjson.loads_arrow(data, schema))
data = fastsimdjson.dumpb_arrow(table)                       # b'{"id":1,"name":"x"}\n...'
data = fastsimdjson.dumpb_arrow(frame, threads=8)            # a polars.DataFrame
```

`loads_arrow(data, schema=None, *, block_size=1048576,
error_on_unknown_fields=False, validate_skipped=True, threads=None)` reads
NDJSON (one object per record) into Arrow record batches. The input is
`bytes`, `bytearray`, `memoryview` or `str`, read in place.
`load_arrow_file(path, ...)` maps the file in memory and reads it the same way
(the file must not shrink meanwhile). Both return a `fastsimdjson.ArrowStream`,
which goes to any library that supports the [Arrow PyCapsule
interface](https://arrow.apache.org/docs/format/CDataInterface/PyCapsuleInterface.html),
without a copy: `pyarrow.table(s)`, `pyarrow.RecordBatchReader.from_stream(s)`,
`polars.DataFrame(s)`, `duckdb.from_arrow(s)`... Like a `RecordBatchReader`,
it hands over its batches once (`ValueError` afterwards); `pyarrow.schema(s)`
gives its schema at any time, and `s.num_rows` the number of records (`None`
once the batches are taken). fastsimdjson does not depend on pyarrow.

* The schema is a `pyarrow.Schema` (or any object with `__arrow_c_schema__`)
  whose fields are booleans, 64-bit integers, unsigned 64-bit integers,
  doubles, strings, structs and lists of these, or null. A field absent from a
  record is null; a key absent from the schema is skipped, or an error with
  `error_on_unknown_fields=True`. Skipped values are checked to be valid
  JSON, unless `validate_skipped=False` (then only their structure is).
* Without a schema, it is inferred as pyarrow's JSON reader does: `null`,
  `true`/`false`, integers that fit in 64 bits (`int64`), other numbers
  (`double`), strings, objects (structs, with the union of their fields in
  order of first appearance) and arrays (lists). A field of nulls takes the
  type of its other values and integers become doubles next to other
  numbers; other mixes of types raise `ValueError`. Strings stay strings (no
  timestamps).
* A new batch starts every `block_size` bytes of input. With `threads=N`
  (`threads=0` or `None`: one per processor), N threads read blocks of
  `block_size` bytes, cut at line ends: a record must then fit on one line.
  When that fails, the input is read again on one thread, so that the errors
  (`ValueError`, with the record number and the field) are the same with any
  number of threads.

`dumpb_arrow(data, *, write_nulls=True, threads=None)` writes Arrow data as
NDJSON `bytes`, one object per row: a `pyarrow.Table`, `RecordBatch`,
`RecordBatchReader` or `StructArray`, a `polars.DataFrame`, a DuckDB
relation, an `ArrowStream`, or any object with `__arrow_c_stream__` or
`__arrow_c_array__`. It writes integers of 8 to 64 bits, floats and doubles
(as `dumpb` writes them, NaN and infinities as `null`), booleans, strings
(including large strings and string views), structs and lists (including
large and fixed-size lists); other types raise `ValueError`. With
`write_nulls=False`, null fields are omitted. With `threads=N`, N threads
write ranges of rows into the result. The output reads back with
`loads_arrow`, and with `loads_many(data, format="lines")`.

These functions release the GIL and run their threads outside Python, with
or without free threading. They are built on
[simdjson_arrow](https://github.com/simdjson/simdjson_arrow), a C++ library
(vendored).

### MessagePack: `json_to_msgpack` and `msgpack_to_json`

```python
packed = fastsimdjson.json_to_msgpack(b'{"id": 1, "tags": ["a", "b"], "score": 2.5}')
# b'\x83\xa2id\x01\xa4tags\x92\xa1a\xa1b\xa5score\xcb@\x04\x00\x00\x00\x00\x00\x00'
msgpack.unpackb(packed)              # {'id': 1, 'tags': ['a', 'b'], 'score': 2.5}
fastsimdjson.msgpack_to_json(packed)  # b'{"id":1,"tags":["a","b"],"score":2.5}'
stream = fastsimdjson.json_to_msgpack(ndjson, ndjson=True, threads=8)  # one value per document
lines = fastsimdjson.msgpack_to_json(stream, stream=True, threads=8)   # NDJSON again
```

`json_to_msgpack(data, *, ndjson=False, block_size=1048576, threads=None)`
converts JSON (`bytes`, `bytearray`, `memoryview` or `str`, read in place) to
MessagePack `bytes` in the most compact encoding: the same bytes as
`msgpack.packb(json.loads(data))` (but for duplicate keys, which
`json.loads` drops, and integers beyond 64 bits). Integers take the smallest integer encoding
(integers beyond 64 bits become doubles), other numbers are doubles, and
strings, arrays and objects take the smallest header for their size; keys keep
their order, duplicates too. With `ndjson=True`, the input holds documents
separated by whitespace (NDJSON, JSON Lines), and each becomes one MessagePack
value, one after the other.

`msgpack_to_json(data, *, stream=False, validate_utf8=True,
binary_as_base64=True, ext_as_null=False, block_size=1048576, threads=None)`
converts MessagePack (any bytes-like object) to compact JSON `bytes`. Floats
get the shortest representation that gives them back, as Python's `repr`
(`1.0`, not `1`; `1e-05`; NaN and infinities as `null`), binary values as base64 strings, map keys that are not strings as
strings (`"1"`, `"true"`, `"null"`); extension values (timestamps too) raise
`ValueError`, or become `null` with `ext_as_null=True`. Strings must be valid
UTF-8 (unless `validate_utf8=False`). With `stream=True`, the input holds a
sequence of values (what `json_to_msgpack(..., ndjson=True)` or several
`msgpack.packb` calls give), and each becomes one line of NDJSON.

* Invalid input raises `ValueError`: `"TAPE_ERROR: ... (document 12)"` for
  JSON, `"truncated string (byte 1234)"` for MessagePack.
* With `threads=N` (`threads=0` or `None`: one per processor), NDJSON and
  streams are converted on N threads, in blocks of about `block_size` bytes.
  A JSON document must then fit on one line; when that fails, the input is
  converted again on one thread, so that errors are the same with any number
  of threads. Single documents use one thread.
* The functions release the GIL and run their threads outside Python, with or
  without free threading, and write their result straight into the `bytes`
  object (no copy). No Python object is made from the data.

They are built on
[simdjson_msgpack](https://github.com/simdjson/simdjson_msgpack), a C++ library
(vendored).

### Threads and free-threaded Python

The module is marked free-threading compatible (`Py_MOD_GIL_NOT_USED` on
Python 3.13 and newer), so importing it on a free-threaded build does not
re-enable the GIL. It is tested with many threads parsing, serializing and
sharing views and stream iterators on free-threaded Python 3.14 and 3.15.

`bytes` and `str` inputs are read in place. A `bytearray` or `memoryview`
could be changed by another thread while we read it, so it is copied first:
always by `loads_many`, `parse_many` and `map_many` (the iterator outlives
the call) and by `loads_arrow`, `json_to_msgpack` and `msgpack_to_json`
(which release the GIL), and only on a free-threaded build by `loads` and
`parse`. Likewise, on a free-threaded build, `dumps` and `dumpb` hold
references to the items of the containers they serialize.

### `release`

`release()` frees the simdjson parser and the string caches kept by the
calling thread. Views returned by `parse` remain valid.

## Build and test

Python 3.10 or newer, and a C++17 compiler (clang, GCC or MSVC). The simdjson
5.1.0 and simdutf 9.2.1 amalgamations, simdjson_arrow 0.1.1 (NDJSON to Arrow
and back: `simdjson_arrow.h` and `simdjson_arrow.cpp`) and simdjson_msgpack
0.1.1 (JSON to MessagePack and back: `simdjson_msgpack.h` and
`simdjson_msgpack.cpp`), both under the Apache-2.0 license, are already in
`vendor/`. simdjson_arrow includes zmij 1.2 (shortest float formatting, MIT
license), which simdjson_msgpack, `dumps` and `dumpb` also use
(`vendor/zmij.h`).
The tests of the Arrow functions (`tests/test_arrow.py`) run when pyarrow is
installed (and polars, for one of them); otherwise they are skipped.

pip:

```sh
python3 -m venv .venv
. .venv/bin/activate
python -m pip install -U pip setuptools
python -m pip install -e ".[test]"
pytest tests
```

uv:

```sh
uv venv
. .venv/bin/activate
uv pip install -e ".[test]"
pytest tests
```

Either one builds the extension and makes `import fastsimdjson` work in the
virtualenv. uv uses the `setuptools` build requirement from `pyproject.toml`,
so it does not need a separate setuptools install for this path.

To compile the extension in the tree instead, install setuptools and pytest
into the same virtualenv, then:

```sh
python -m pip install setuptools pytest
python setup.py build_ext --inplace
PYTHONPATH=src python -m pytest tests
```

```sh
uv pip install setuptools pytest
python setup.py build_ext --inplace
PYTHONPATH=src python -m pytest tests
```

Recent setuptools copies the `.so` next to `src/fastsimdjson.cpp`, which is
why `PYTHONPATH=src` is required for the in-place build.

`tests/test_loads.py` compares `loads` with `json.loads` on types and key
order; `tests/test_lazy.py` checks the views returned by `parse` the same way,
`tests/test_dumps.py` compares `dumps` with `json.dumps` and `dumpb` with
orjson (output and exceptions; the orjson comparisons are skipped when orjson
is not installed, and recorded orjson results are checked either way), and
`tests/test_stream.py` and `tests/test_files.py` cover streams and files. It
covers scalars, integers past 64 bits, UTF-8 strings at every length from 0 to
199, the key cache, random documents, rejected input, deep
nesting, padding at a page boundary, a saturated array count, reference
counts, and release of a parser that has grown past 64 MB. The corpus test
is skipped until simdjson-data is checked out beside the project:

```sh
git clone --depth 1 https://github.com/simdjson/simdjson-data.git
pytest tests
# or: JSONDIR=/path/to/jsonexamples pytest tests
```

The suite builds an ~80 MB document and a list of 16,777,221 integers, so
give it some RAM.

The benchmark scripts need simdjson-data and the `bench` extra (orjson,
jiter, msgspec, pysimdjson, cysimdjson). `bench.py` times `loads` against
`json.loads`, orjson and jiter, `bench_lazy.py` times `parse` against pysimdjson and
cysimdjson, `bench_dumps.py` times `dumps` against `json.dumps` and `dumpb`
against orjson and msgspec, and `bench_many.py` times `loads_many`.

pip:

```sh
python -m pip install -e ".[bench]"
python bench.py
python bench_lazy.py
python bench_dumps.py
python bench_many.py
```

uv:

```sh
uv pip install -e ".[bench]"
python bench.py
python bench_lazy.py
python bench_dumps.py
python bench_many.py
```

## Benchmarks

Intel Xeon Gold 6548N (Emerald Rapids), one core, Python 3.14.6,
fastsimdjson 0.3.0 (simdjson 5.0.2), the 22 files of
[simdjson-data](https://github.com/simdjson/simdjson-data). The scripts are in
this repository and in
[the blog repository](https://github.com/lemire/Code-used-on-Daniel-Lemire-s-blog/tree/master/2026/09/pysimdjson).

### Whole documents

Each parser produces the whole document as Python objects. Speed is the
geometric mean over the 22 files (higher is better).

<picture>
  <source media="(prefers-color-scheme: dark)" srcset="https://raw.githubusercontent.com/simdjson/fastpysimdjson/main/doc/perf_loads_dark.png">
  <img src="https://raw.githubusercontent.com/simdjson/fastpysimdjson/main/doc/perf_loads.png" width="80%" alt="Parsing JSON in Python: fastsimdjson loads 0.78 GB/s, orjson 0.60, msgspec 0.53, jiter 0.48, jiter without its string cache 0.44, pysimdjson 0.44, cysimdjson 0.43, ujson 0.37, python-rapidjson 0.24, simplejson 0.23, json 0.22">
</picture>

| parser | GB/s | vs `json.loads` |
|---|---:|---:|
| json (standard library) | 0.22 | 1.00× |
| simplejson 4.2.0 | 0.23 | 1.05× |
| python-rapidjson 1.25 | 0.24 | 1.10× |
| ujson 6.0.0 | 0.37 | 1.66× |
| cysimdjson 26.27 | 0.43 | 1.96× |
| pysimdjson 7.0.2 | 0.44 | 1.98× |
| jiter 0.17.0 (no string cache) | 0.44 | 2.00× |
| jiter 0.17.0 (string cache, the default) | 0.48 | 2.21× |
| msgspec 0.22.0 | 0.53 | 2.42× |
| orjson 3.12.0 | 0.60 | 2.74× |
| **fastsimdjson `loads`** | **0.78** | **3.53×** |

fastsimdjson is the fastest on 21 of the 22 files, and ties with orjson on
`numbers.json`, an array of floating-point numbers (within 2%). On numbers,
both spend most of their time creating Python floats, which costs the same in
both: simdjson parses the numbers of these files 15% to 30% faster than
orjson's parser (yyjson), but that is a small part of the total. Part of the
gain comes from pausing the garbage collector while the objects are built.
Timing each call on its own, fastsimdjson's lead over orjson is 1.33× with
the collector enabled and 1.22× with it disabled (geometric means); without
the collector, `canada.json`, `mesh.json` and `numbers.json` are ties. yyjson 4.0.6 is left out: it
returns wrong strings for non-ASCII text. jiter (the parser of pydantic)
caches strings by default; turning the cache off (`cache_mode=False`) makes it
slower here.

Parsing is no longer the bottleneck. simdjson alone parses these files at
3.1 GB/s. It accounts for about a third of the time of `loads`; the rest goes
into creating Python objects. Freeing those objects later costs about a sixth
of the total. Even if parsing took no time at all, `loads` would be less than
1.5 times faster.

### Parts of documents with `parse`

If you only need a few values, `parse` creates only those. Extracting the id
and the screen name of the 100 statuses of `twitter.json`:

<picture>
  <source media="(prefers-color-scheme: dark)" srcset="https://raw.githubusercontent.com/simdjson/fastpysimdjson/main/doc/perf_parse_dark.png">
  <img src="https://raw.githubusercontent.com/simdjson/fastpysimdjson/main/doc/perf_parse.png" width="80%" alt="Reading part of twitter.json: fastsimdjson parse 158 µs, pysimdjson 179, cysimdjson 229, msgspec 336, fastsimdjson loads 861, orjson 1009, jiter 1281, jiter without its string cache 1592, json 3922">
</picture>

| method | µs |
|---|---:|
| `json.loads` | 3922 |
| jiter (no string cache) | 1592 |
| jiter (string cache) | 1281 |
| orjson | 1009 |
| fastsimdjson `loads` | 861 |
| msgspec (typed `Struct`) | 336 |
| cysimdjson (lazy) | 229 |
| pysimdjson (lazy) | 179 |
| **fastsimdjson `parse`** | **158** |

Here `parse` is about 25 times faster than `json.loads` and 5.5 times faster
than `loads`. Most of its time is the simdjson parse itself.

### `dumps` against `json.dumps`

<picture>
  <source media="(prefers-color-scheme: dark)" srcset="https://raw.githubusercontent.com/simdjson/fastpysimdjson/main/doc/perf_dumps_dark.png">
  <img src="https://raw.githubusercontent.com/simdjson/fastpysimdjson/main/doc/perf_dumps.png" width="80%" alt="Writing JSON as str: fastsimdjson dumps 4.63 times faster than json.dumps">
</picture>

`dumps` returns exactly the `str` of `json.dumps` and is 4.6 times faster
(geometric mean over the 22 files; from 3.1 times on `citm_catalog.json` to
12 times on `numbers.json`).

### `dumpb` against orjson and msgspec

`dumpb`, `orjson.dumps` and `msgspec.json.encode` produce compact UTF-8
`bytes`; `dumpb` and orjson produce identical bytes on every file. With the
standard library, the same compact bytes come from
`json.dumps(obj, separators=(",", ":"), ensure_ascii=False).encode()`.
Microseconds:

<picture>
  <source media="(prefers-color-scheme: dark)" srcset="https://raw.githubusercontent.com/simdjson/fastpysimdjson/main/doc/perf_dumpb_dark.png">
  <img src="https://raw.githubusercontent.com/simdjson/fastpysimdjson/main/doc/perf_dumpb.png" width="80%" alt="Writing JSON as bytes, speed relative to json.dumps(...).encode(): fastsimdjson dumpb 10.09x, orjson 9.69x, msgspec 6.03x">
</picture>

| file | `json.dumps(...).encode()` | fastsimdjson `dumpb` | orjson | msgspec |
|---|---:|---:|---:|---:|
| twitter | 1827 | 202 | 204 | 330 |
| citm_catalog | 2921 | 432 | 432 | 499 |
| github_events | 177 | 17 | 19 | 31 |
| gsoc-2018 | 15386 | 595 | 554 | 1462 |
| update-center | 2482 | 254 | 232 | 477 |
| canada | 38101 | 2432 | 2936 | 3640 |
| mesh | 8726 | 785 | 999 | 1370 |
| numbers | 2530 | 178 | 200 | 332 |

`dumpb` and orjson are on par: over the 22 files, `dumpb` is 4% faster
(geometric mean), from 10% slower on text-heavy or tiny files
(`gsoc-2018.json`, `update-center.json`, `repeat.json`) to 27% faster on files
full of numbers. Both are 1.7
times faster than msgspec and 10 times faster than `json`. The comparison was
run on a processor with AVX-512, which `dumpb` and orjson both use to escape
strings; other x64 processors use SSE2 and ARM processors NEON.

### Writing NDJSON with `dumpb_many`

The 593,945 events of two hours of [GH Archive](https://www.gharchive.org/)
(1.87 GB of NDJSON), written back as NDJSON from Python objects. With the
GIL (Python 3.14.6), one thread: `orjson.dumps` on each record, joined, runs
at 0.94 GB/s, `dumpb` on each record at 1.07 GB/s, and `dumpb_many` at
1.41 GB/s. On free-threaded Python 3.14.6t, GB/s:

| threads | 1 | 8 | 32 | 64 |
|---|---:|---:|---:|---:|
| `dumpb_many(records, threads=N)` | 1.3 | 6.0 | 11 | 12 |

### NDJSON to Arrow and back

Two files: 4 million flat records (611 MB, 9 scalar fields), and 304,929 GH
Archive events (886 MB), read with a schema of 6 of their fields (two of them
structs; the rest of each event is skipped), or inferring the schema. Reading
gives a `pyarrow.Table` (`pyarrow.table(fastsimdjson.loads_arrow(data,
schema, threads=N))`); the time includes the import into pyarrow. Writing
starts from that table: fastsimdjson `dumpb_arrow`, polars `write_ndjson` and
DuckDB `COPY ... TO` (on tmpfs). Python 3.13, pyarrow 26.0, polars 2.1, DuckDB
2.0, simdjson 5.1.0, Intel Xeon Gold 6548N (up to 32 threads on the 32 cores
of one socket; 64 threads on both sockets).

<picture>
  <source media="(prefers-color-scheme: dark)" srcset="https://raw.githubusercontent.com/simdjson/fastpysimdjson/main/doc/perf_arrow_read_1_dark.png">
  <img src="https://raw.githubusercontent.com/simdjson/fastpysimdjson/main/doc/perf_arrow_read_1.png" width="80%" alt="Reading NDJSON into a pyarrow.Table on one thread: flat, fastsimdjson 0.70 GB/s, pyarrow 0.18, polars 0.27; GH Archive, fastsimdjson 1.95, pyarrow 0.45, polars 0.53; flat with an inferred schema, fastsimdjson 0.72, pyarrow 0.17, polars 0.08">
</picture>

<picture>
  <source media="(prefers-color-scheme: dark)" srcset="https://raw.githubusercontent.com/simdjson/fastpysimdjson/main/doc/perf_arrow_read_32_dark.png">
  <img src="https://raw.githubusercontent.com/simdjson/fastpysimdjson/main/doc/perf_arrow_read_32.png" width="80%" alt="Reading NDJSON into a pyarrow.Table on 32 threads: flat, fastsimdjson 13.9 GB/s, pyarrow 5.1, polars 4.0; GH Archive, fastsimdjson 28.4, pyarrow 12.1, polars 3.8; flat with an inferred schema, fastsimdjson 13.3, pyarrow 4.8, polars 0.12">
</picture>

<picture>
  <source media="(prefers-color-scheme: dark)" srcset="https://raw.githubusercontent.com/simdjson/fastpysimdjson/main/doc/perf_arrow_write_1_dark.png">
  <img src="https://raw.githubusercontent.com/simdjson/fastpysimdjson/main/doc/perf_arrow_write_1.png" width="80%" alt="Writing a pyarrow.Table as NDJSON on one thread: flat, fastsimdjson 0.96 GB/s, polars 0.46, DuckDB 0.30; GH Archive, fastsimdjson 1.07, polars 0.47, DuckDB 0.31">
</picture>

<picture>
  <source media="(prefers-color-scheme: dark)" srcset="https://raw.githubusercontent.com/simdjson/fastpysimdjson/main/doc/perf_arrow_write_32_dark.png">
  <img src="https://raw.githubusercontent.com/simdjson/fastpysimdjson/main/doc/perf_arrow_write_32.png" width="80%" alt="Writing a pyarrow.Table as NDJSON on 32 threads: flat, fastsimdjson 7.8 GB/s, polars 1.3, DuckDB 2.8; GH Archive, fastsimdjson 4.7, polars 1.1, DuckDB 0.79">
</picture>

All the numbers, in GB/s:

| | threads | 1 | 8 | 32 | 64 |
|---|---|---:|---:|---:|---:|
| flat, read | fastsimdjson `loads_arrow` | **0.70** | **5.3** | **13.9** | **16.2** |
| | pyarrow `read_json` | 0.18 | 1.4 | 5.1 | 7.7 |
| | polars `read_ndjson` | 0.27 | 2.0 | 4.0 | 2.8 |
| flat, read, inferring the schema | fastsimdjson `loads_arrow` | **0.72** | **5.2** | **13.3** | **12.7** |
| | pyarrow `read_json` | 0.17 | 1.3 | 4.8 | 6.6 |
| | polars `read_ndjson` | 0.08 | 0.12 | 0.12 | 0.12 |
| GH Archive, read | fastsimdjson `loads_arrow` | **1.95** | **12.9** | **28.4** | **25.3** |
| | pyarrow `read_json` | 0.45 | 3.5 | 12.1 | 15.4 |
| | polars `read_ndjson` | 0.53 | 3.9 | 3.8 | 3.0 |
| flat, write | fastsimdjson `dumpb_arrow` | **0.96** | **3.9** | **7.8** | **12.6** |
| | polars | 0.46 | 1.1 | 1.3 | 1.6 |
| | DuckDB | 0.30 | 1.8 | 2.8 | 2.3 |
| GH Archive, write | fastsimdjson `dumpb_arrow` | **1.07** | **2.9** | **4.7** | **4.5** |
| | polars | 0.47 | 0.78 | 1.1 | 0.91 |
| | DuckDB | 0.31 | 0.95 | 0.79 | 0.66 |

`load_arrow_file` on the same files, from the page cache, reads at 0.67 and
1.6 GB/s on one thread, 9.8 and 14.8 GB/s on 32 threads. The benchmark is
`THREADS=N python bench_arrow.py flat.ndjson:flat gharchive.ndjson:gh
flat.ndjson:infer`; `path:infer` times any NDJSON file without a schema. The
flat file comes from
[simdjson_arrow's `benchmarks/gendata.py`](https://github.com/simdjson/simdjson_arrow/blob/main/benchmarks/gendata.py)
and the events from GH Archive
(`curl -sSfL https://data.gharchive.org/2024-03-13-12.json.gz | gunzip > gharchive.ndjson`).
The charts come from `python doc/make_charts.py arrow`.

### JSON to MessagePack and back

From JSON bytes to MessagePack bytes, and back, against the usual ways in
Python, which decode into Python objects, then encode them:
`ormsgpack.packb(orjson.loads(s))`, `msgspec.msgpack.encode(msgspec.json.decode(s))`
and so on. The 22 files of simdjson-data, geometric means of the speeds, in
GB/s of JSON (read when writing MessagePack, written when writing JSON).
Python 3.14, msgpack 1.2, orjson 3.13, ormsgpack 1.13, msgspec 0.22, Intel
Xeon Gold 6338 (Ice Lake), one core.

<picture>
  <source media="(prefers-color-scheme: dark)" srcset="https://raw.githubusercontent.com/simdjson/fastpysimdjson/main/doc/perf_msgpack_encode_dark.png">
  <img src="https://raw.githubusercontent.com/simdjson/fastpysimdjson/main/doc/perf_msgpack_encode.png" width="80%" alt="JSON to MessagePack in Python, geometric mean over the 22 files of simdjson-data: fastsimdjson json_to_msgpack 1.37 GB/s, orjson with ormsgpack 0.35, msgspec 0.34, fastsimdjson loads with msgpack 0.28, orjson with msgpack 0.25, json with msgpack 0.13">
</picture>

<picture>
  <source media="(prefers-color-scheme: dark)" srcset="https://raw.githubusercontent.com/simdjson/fastpysimdjson/main/doc/perf_msgpack_decode_dark.png">
  <img src="https://raw.githubusercontent.com/simdjson/fastpysimdjson/main/doc/perf_msgpack_decode.png" width="80%" alt="MessagePack to JSON in Python, geometric mean over the 22 files of simdjson-data: fastsimdjson msgpack_to_json 0.94 GB/s, ormsgpack with orjson 0.27, msgspec 0.25, msgpack with orjson 0.20, msgpack with fastsimdjson dumpb 0.20, msgpack with json 0.07">
</picture>

Streams: the 304,929 events of an hour of GH Archive (886 MB of NDJSON), with
`json_to_msgpack(data, ndjson=True, threads=N)` and
`msgpack_to_json(packed, stream=True, threads=N)`, against orjson with
ormsgpack (each document) and `msgpack.Unpacker` with orjson (each value), on
one thread (one thread per core of one socket up to 32 threads, then both
sockets):

<picture>
  <source media="(prefers-color-scheme: dark)" srcset="https://raw.githubusercontent.com/simdjson/fastpysimdjson/main/doc/perf_msgpack_ndjson_dark.png">
  <img src="https://raw.githubusercontent.com/simdjson/fastpysimdjson/main/doc/perf_msgpack_ndjson.png" width="80%" alt="GH Archive NDJSON to MessagePack: fastsimdjson 15.0 GB/s on 64 threads, 10.8 on 32, 6.8 on 8, 1.18 on one thread, orjson with ormsgpack 0.29 on one thread; MessagePack to NDJSON: fastsimdjson 5.0 GB/s on 8 threads, 1.05 on one thread, msgpack with orjson 0.20">
</picture>

| threads | 1 | 2 | 4 | 8 | 16 | 32 | 64 |
|---|---:|---:|---:|---:|---:|---:|---:|
| NDJSON to MessagePack | 1.18 | 1.88 | 4.2 | 6.8 | 8.6 | 10.8 | 15.0 |
| MessagePack to NDJSON | 1.05 | 1.67 | 3.3 | 5.0 | 3.4 | 4.2 | 3.5 |

MessagePack to NDJSON stops scaling at about eight threads: one thread must
find where the values end (MessagePack has no line ends) before the others
can convert them. The benchmark is `python bench_msgpack.py` (the files of
`JSONDIR`, by default `simdjson-data/jsonexamples`) and
`python bench_msgpack.py --ndjson gharchive.ndjson 1 8 32`; the charts come
from `python doc/make_charts.py msgpack`.

### Streams with `loads_many`

20 MB of NDJSON (5268 objects and arrays, one per line), made from the same
files; best of three runs:

<picture>
  <source media="(prefers-color-scheme: dark)" srcset="https://raw.githubusercontent.com/simdjson/fastpysimdjson/main/doc/perf_ndjson_dark.png">
  <img src="https://raw.githubusercontent.com/simdjson/fastpysimdjson/main/doc/perf_ndjson.png" width="80%" alt="Reading NDJSON: fastsimdjson parse_many 0.94 GB/s, loads_many 0.38, msgspec decode_lines 0.37, fastsimdjson loads per line 0.33, msgspec decode per line 0.27, orjson per line 0.26, jiter per line 0.22, jiter without its string cache per line 0.20, json per line 0.12">
</picture>

| method | ms | GB/s |
|---|---:|---:|
| `json.loads` on each line | 169 | 0.12 |
| jiter on each line (no string cache) | 98 | 0.20 |
| jiter on each line (string cache) | 90 | 0.22 |
| orjson on each line | 77 | 0.26 |
| msgspec `decode` on each line | 74 | 0.27 |
| fastsimdjson `loads` on each line | 60 | 0.33 |
| msgspec `decode_lines` | 53 | 0.37 |
| fastsimdjson `loads_many` | 52 | 0.38 |
| fastsimdjson `parse_many` (views only) | 21 | 0.94 |

msgspec's `decode_lines` and `loads_many` are on par (`loads_many` is about 2%
faster). `parse_many` only creates the views; reading values from them adds to
its time.

### Streams on several threads

300 MB of NDJSON (146,624 records: tweets, GitHub events, citm performances,
gsoc and update-center entries), free-threaded Python 3.14.6, collector
disabled (see below), Intel Xeon Gold 6548N (64 cores); GB/s:

| threads | 1 | 2 | 4 | 8 | 16 | 32 | 64 |
|---|---:|---:|---:|---:|---:|---:|---:|
| `map_many(len, data, threads=N)` | 1.03 | 1.78 | 3.66 | 6.82 | 13.0 | 23.7 | **28.8** |
| `parse_many(data, threads=N)` | 2.00 | 2.84 | 5.28 | 9.26 | 16.1 | 22.7 | 25.8 |
| `loads_many(data, threads=N)` | 0.90 | 1.44 | 2.79 | 4.99 | 8.98 | 13.3 | 15.4 |

`map_many` is 28 times faster on 64 threads than on one (up to 38 GB/s when
nothing else runs in the process). `loads_many` builds every document for
the calling thread and is 17 times faster. With the collector enabled,
Python collects after the call returns: it examines the millions of objects
just created, which takes about 0.1 s here, with or without threads.

## Limitations

* The simdjson parser and the key and string caches are thread-local.
  `release()` frees the parser and the cached strings retained by the calling
  thread, and they are freed when a Python thread exits. A parser that grows
  past 64 MB is freed on its own at the end of that call; its caches stay.
* Subinterpreters are not supported: the module keeps per-process state (its
  types, and per-thread parsers and string caches).
