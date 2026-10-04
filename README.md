# fastsimdjson

A Python binding for [simdjson](https://github.com/simdjson/simdjson) that
parses JSON into native Python objects (`dict`, `list`, `str`, `int`,
`float`, `bool`, `None`) and serializes them back. It is a drop-in
replacement for the `json` module's `loads`, `load`, `dumps` and `dump`, and
it can be over 3 times faster than the standard `json.loads` and
`json.dumps`. When you only need part of a document, its lazy `parse`
function is faster still. It also reads streams of documents (NDJSON, JSON
Lines) and writes JSON as `bytes` (`dumpb`).

With pip:

```sh
pip install fastsimdjson
```

With uv:

```sh
uv pip install fastsimdjson      # in a uv project: uv add fastsimdjson
```

Wheels are available for Linux, macOS and Windows, for Python 3.10 to 3.14,
including free-threaded Python 3.14.

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

### `dumps`, `dumpb` and `dump`: writing JSON

```python
fastsimdjson.dumps({"a": [1, 2.5, None]})            # '{"a": [1, 2.5, null]}'
fastsimdjson.dumps(obj, indent=2, sort_keys=True)
fastsimdjson.dumpb(obj, separators=(",", ":"), ensure_ascii=False)  # bytes
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
serialize. `dumpb(obj, **kw)` returns the same text as UTF-8 `bytes`, exactly
`json.dumps(obj, **kw).encode()`, without building a `str` first: use it to
write to a binary file or a socket. `dump(obj, fp, **kw)` writes
`dumps(obj, **kw)` to `fp`.

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

### `release`

`release()` frees the simdjson parser and the string caches kept by the
calling thread. Views returned by `parse` remain valid.

## Build and test

Python 3.10 or newer, and a C++17 compiler (clang, GCC or MSVC). The simdjson 5.0.2
and simdutf 9.2.1 amalgamations are already in `vendor/`.

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
`tests/test_dumps.py` compares `dumps` and `dumpb` with `json.dumps` (output
and exceptions), and `tests/test_stream.py` and `tests/test_files.py` cover
streams and files. It covers scalars, integers past 64 bits, UTF-8 strings at every
length from 0 to 199, the key cache, random documents, rejected input, deep
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
msgspec, pysimdjson, cysimdjson). `bench.py` times `loads` against
`json.loads` and orjson, `bench_lazy.py` times `parse` against pysimdjson and
cysimdjson, `bench_dumps.py` times `dumps` and `dumpb`, and `bench_many.py`
times `loads_many`.

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
fastsimdjson 0.3.0 (development version, simdjson 5.0.2), the 22 files of
[simdjson-data](https://github.com/simdjson/simdjson-data). The scripts are in
this repository and in
[the blog repository](https://github.com/lemire/Code-used-on-Daniel-Lemire-s-blog/tree/master/2026/09/pysimdjson).

### Whole documents

Each parser produces the whole document as Python objects. Speed is the
geometric mean over the 22 files (higher is better).

| parser | GB/s | vs `json.loads` |
|---|---:|---:|
| json (standard library) | 0.22 | 1.00× |
| simplejson 4.2.0 | 0.23 | 1.05× |
| python-rapidjson 1.25 | 0.24 | 1.10× |
| ujson 6.0.0 | 0.37 | 1.66× |
| cysimdjson 26.27 | 0.43 | 1.96× |
| pysimdjson 7.0.2 | 0.44 | 1.98× |
| msgspec 0.22.0 | 0.53 | 2.42× |
| orjson 3.12.0 | 0.60 | 2.74× |
| **fastsimdjson `loads`** | **0.78** | **3.53×** |

fastsimdjson is the fastest on 21 of the 22 files; orjson is slightly faster
on `numbers.json`, an array of floating-point numbers. Part of the gain comes
from pausing the garbage collector while the objects are built: if the
collector is disabled for every parser, fastsimdjson's lead over orjson drops
from 1.29× to 1.17×, and orjson is slightly faster on `canada.json`,
`mesh.json` and `numbers.json`. yyjson 4.0.6 is left out: it returns wrong
strings for non-ASCII text.

Parsing is no longer the bottleneck. simdjson alone parses these files at
3.1 GB/s. It accounts for about a third of the time of `loads`; the rest goes
into creating Python objects. Freeing those objects later costs about a sixth
of the total. Even if parsing took no time at all, `loads` would be less than
1.5 times faster.

### Parts of documents with `parse`

If you only need a few values, `parse` creates only those. Extracting the id
and the screen name of the 100 statuses of `twitter.json`:

| method | µs |
|---|---:|
| `json.loads` | 3922 |
| orjson | 1009 |
| fastsimdjson `loads` | 861 |
| msgspec (typed `Struct`) | 336 |
| cysimdjson (lazy) | 229 |
| pysimdjson (lazy) | 179 |
| **fastsimdjson `parse`** | **158** |

Here `parse` is about 25 times faster than `json.loads` and 5.5 times faster
than `loads`. Most of its time is the simdjson parse itself.

### Writing JSON with `dumps` and `dumpb`

With the default arguments, `dumps` returns exactly the `str` of `json.dumps`
and is 3.3 times faster (geometric mean over the 22 files; from 2.3 times on
text-heavy files to 8 times on files full of numbers).

orjson and msgspec produce compact UTF-8 `bytes`: no spaces after separators,
non-ASCII characters left as they are. The fair comparison is with the same
output: `dumpb(obj, separators=(",", ":"), ensure_ascii=False)`, and
`json.dumps` with the same arguments followed by `.encode()`. These four
produce identical bytes on 18 of the 22 files; on the others they differ only
in how some floats are written (`1e-05` as in Python's `repr`, against
`0.00001`). Microseconds:

| file | `json.dumps(...).encode()` | fastsimdjson `dumpb` | orjson | msgspec |
|---|---:|---:|---:|---:|
| twitter | 1798 | 549 | 200 | 348 |
| citm_catalog | 2956 | 1115 | 430 | 493 |
| github_events | 173 | 45 | 18 | 30 |
| gsoc-2018 | 15309 | 1848 | 546 | 1423 |
| canada | 38095 | 4708 | 2921 | 3636 |
| numbers | 2515 | 352 | 197 | 333 |

`dumpb` is 3.8 times faster than `json.dumps(...).encode()` (geometric mean;
2.4 to 8.3 times). orjson is faster still, by 2.6 times, and msgspec by 1.6
times (geometric means). Unlike them, `dumps` and `dumpb` accept every argument
of `json.dumps` and produce its exact output.

### Streams with `loads_many`

20 MB of NDJSON (5268 objects and arrays, one per line), made from the same
files; best of three runs:

| method | ms | GB/s |
|---|---:|---:|
| `json.loads` on each line | 169 | 0.12 |
| orjson on each line | 77 | 0.26 |
| msgspec `decode` on each line | 74 | 0.27 |
| fastsimdjson `loads` on each line | 60 | 0.33 |
| msgspec `decode_lines` | 53 | 0.37 |
| fastsimdjson `loads_many` | 52 | 0.38 |
| fastsimdjson `parse_many` (views only) | 21 | 0.94 |

msgspec's `decode_lines` and `loads_many` are on par (`loads_many` is about 2%
faster). `parse_many` only creates the views; reading values from them adds to
its time.

## Limitations

* The simdjson parser and the key and string caches are thread-local.
  `release()` frees the parser and the cached strings retained by the calling
  thread. A parser that grows past 64 MB is freed on its own at the end of
  that call; its caches stay. A thread that exits without `release()` leaves
  its cached strings behind.
* The module is marked free-threading compatible (`Py_MOD_GIL_NOT_USED` on
  Python 3.13 and newer), so importing it on a free-threaded build does not
  re-enable the GIL. On a free-threaded build, `bytearray` and `memoryview`
  inputs are copied before parsing. Subinterpreters are not supported.
