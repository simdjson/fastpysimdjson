# fastsimdjson

A Python binding for [simdjson](https://github.com/simdjson/simdjson) that
parses JSON into native Python objects (`dict`, `list`, `str`, `int`,
`float`, `bool`, `None`) and serializes them back. It is a drop-in
replacement for the `json` module's `loads`, `load`, `dumps` and `dump`, and
it can be over 3 times faster than the standard `json.loads` and
`json.dumps`. When you only need part of a document, its lazy `parse`
function is faster still. It also reads streams of documents (NDJSON, JSON
Lines). It writes JSON in two ways: `dumps` returns the same `str` as
`json.dumps`, and `dumpb` returns the same `bytes` as `orjson.dumps`, as fast
as orjson.

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

Python 3.10 or newer, and a C++17 compiler (clang, GCC or MSVC). The simdjson
5.0.2 and simdutf 9.2.1 amalgamations, and zmij 1.2 (shortest float
formatting, MIT license), are already in `vendor/`.

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
msgspec, pysimdjson, cysimdjson). `bench.py` times `loads` against
`json.loads` and orjson, `bench_lazy.py` times `parse` against pysimdjson and
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
  <img src="https://raw.githubusercontent.com/simdjson/fastpysimdjson/main/doc/perf_loads.png" width="80%" alt="Parsing JSON in Python: fastsimdjson loads 0.78 GB/s, orjson 0.60, msgspec 0.53, pysimdjson 0.44, cysimdjson 0.43, ujson 0.37, python-rapidjson 0.24, simplejson 0.23, json 0.22">
</picture>

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

fastsimdjson is the fastest on 21 of the 22 files, and ties with orjson on
`numbers.json`, an array of floating-point numbers (within 2%). On numbers,
both spend most of their time creating Python floats, which costs the same in
both: simdjson parses the numbers of these files 15% to 30% faster than
orjson's parser (yyjson), but that is a small part of the total. Part of the
gain comes from pausing the garbage collector while the objects are built.
Timing each call on its own, fastsimdjson's lead over orjson is 1.33× with
the collector enabled and 1.22× with it disabled (geometric means); without
the collector, `canada.json`, `mesh.json` and `numbers.json` are ties. yyjson 4.0.6 is left out: it
returns wrong strings for non-ASCII text.

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
  <img src="https://raw.githubusercontent.com/simdjson/fastpysimdjson/main/doc/perf_parse.png" width="80%" alt="Reading part of twitter.json: fastsimdjson parse 158 µs, pysimdjson 179, cysimdjson 229, msgspec 336, fastsimdjson loads 861, orjson 1009, json 3922">
</picture>

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

### Streams with `loads_many`

20 MB of NDJSON (5268 objects and arrays, one per line), made from the same
files; best of three runs:

<picture>
  <source media="(prefers-color-scheme: dark)" srcset="https://raw.githubusercontent.com/simdjson/fastpysimdjson/main/doc/perf_ndjson_dark.png">
  <img src="https://raw.githubusercontent.com/simdjson/fastpysimdjson/main/doc/perf_ndjson.png" width="80%" alt="Reading NDJSON: fastsimdjson parse_many 0.94 GB/s, loads_many 0.38, msgspec decode_lines 0.37, fastsimdjson loads per line 0.33, msgspec decode per line 0.27, orjson per line 0.26, json per line 0.12">
</picture>

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
  thread, and they are freed when a Python thread exits. A parser that grows
  past 64 MB is freed on its own at the end of that call; its caches stay.
* The module is marked free-threading compatible (`Py_MOD_GIL_NOT_USED` on
  Python 3.13 and newer), so importing it on a free-threaded build does not
  re-enable the GIL. It is tested with many threads parsing, serializing and
  sharing views and stream iterators on free-threaded Python 3.14 and 3.15.
  On a free-threaded build, `bytearray` and `memoryview` inputs are copied
  before parsing, and `dumps` and `dumpb` hold references to the items of the
  containers they serialize, since other threads may change them.
  Subinterpreters are not supported.
