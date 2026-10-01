# fastsimdjson

A Python binding for [simdjson](https://github.com/simdjson/simdjson) that
parses JSON into native Python objects (`dict`, `list`, `str`, `int`,
`float`, `bool`, `None`). It is a drop-in replacement for `json.loads`, and
it can be over 3 times faster than the standard `json.loads`. When you only
need part of a document, its lazy `parse` function is faster still.

```sh
pip install fastsimdjson
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
order; `tests/test_lazy.py` checks the views returned by `parse` the same way. It covers scalars, integers past 64 bits, UTF-8 strings at every
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

To time `loads` against `json.loads` and orjson on those files
(`bench_lazy.py` times `parse` against pysimdjson and cysimdjson):

```sh
python -m pip install -e ".[bench]"
python bench.py
```

```sh
uv pip install -e ".[bench]"
python bench.py
```

## Benchmarks

Intel Xeon Gold 6548N (Emerald Rapids), one core, Python 3.14.6,
fastsimdjson 0.2.0, the 22 files of
[simdjson-data](https://github.com/simdjson/simdjson-data). The scripts and
the full results are in
[the blog repository](https://github.com/lemire/Code-used-on-Daniel-Lemire-s-blog/tree/master/2026/09/pysimdjson).

### Whole documents

Each parser produces the whole document as Python objects. Speed is the
geometric mean over the 22 files (higher is better).

| parser | GB/s | vs `json.loads` |
|---|---:|---:|
| json (standard library) | 0.22 | 1.00× |
| simplejson 4.1.2 | 0.23 | 1.05× |
| python-rapidjson 1.25 | 0.24 | 1.10× |
| ujson 6.0.0 | 0.36 | 1.65× |
| cysimdjson 26.27 | 0.43 | 1.94× |
| pysimdjson 7.0.2 | 0.44 | 1.98× |
| msgspec 0.22.0 | 0.53 | 2.41× |
| orjson 3.12.0 | 0.60 | 2.73× |
| **fastsimdjson `loads`** | **0.77** | **3.49×** |

fastsimdjson is the fastest on 21 of the 22 files; orjson is slightly faster
on `numbers.json`, an array of floating-point numbers. Part of the gain comes
from pausing the garbage collector while the objects are built: if the
collector is disabled for every parser, fastsimdjson's lead over orjson drops
from 1.28× to 1.18×. yyjson 4.0.6 is left out: it returns wrong strings for
non-ASCII text.

Parsing is no longer the bottleneck. simdjson alone parses these files at
3.0 GB/s. It accounts for about a third of the time of `loads`; the rest goes
into creating Python objects. Freeing those objects later costs about a sixth
of the total. Even if parsing took no time at all, `loads` would be less than
1.5 times faster.

### Parts of documents with `parse`

If you only need a few values, `parse` creates only those. Extracting the id
and the screen name of the 100 statuses of `twitter.json`:

| method | µs |
|---|---:|
| `json.loads` | 3879 |
| orjson | 1008 |
| fastsimdjson `loads` | 860 |
| msgspec (typed `Struct`) | 336 |
| cysimdjson (lazy) | 235 |
| pysimdjson (lazy) | 183 |
| **fastsimdjson `parse`** | **155** |

Here `parse` is 25 times faster than `json.loads` and 5.5 times faster than
`loads`. Most of its time is the simdjson parse itself: reading the 200
values takes less than 20 µs. Compared with pysimdjson on other tasks
(µs, lower is better):

| file | task | fastsimdjson `parse` | fastsimdjson `loads` | pysimdjson |
|---|---|---:|---:|---:|
| twitter | open | 140 | 676 | 156 |
| citm_catalog | open | 369 | 1612 | 472 |
| citm_catalog | extract | 394 | 2142 | 504 |
| gsoc-2018 | open | 615 | 2206 | 813 |
| twitter | visit all | 1985 | 2183 | 3033 |
| canada | visit all | 17528 | 18998 | 19750 |
| twitter_api_response | open | 3.6 | 13.6 | 3.4 |

"open" parses the document and looks at its root; "extract" collects the
start time of every performance; "visit all" walks every value through the
views (with `loads`: through the dict). When you visit everything, `parse`
is about as fast as `loads`. On very small documents (15 KB), pysimdjson's
`parse` is marginally faster.

## Limitations

* There is no `dumps`: fastsimdjson only parses JSON.
* The simdjson parser and the key and string caches are thread-local.
  `release()` frees the parser and the cached strings retained by the calling
  thread. A parser that grows past 64 MB is freed on its own at the end of
  that call; its caches stay. A thread that exits without `release()` leaves
  its cached strings behind. The module is marked free-threading compatible
  (`Py_MOD_GIL_NOT_USED` on Python 3.13 and newer), so importing it on a
  free-threaded build does not re-enable the GIL. Subinterpreters are not
  supported. On a free-threaded build, `bytearray` and `memoryview` inputs
  are copied before parsing.
* A document simdjson rejects is reparsed with `json.loads`. `loads` returns
  that value when `json.loads` accepts it, which is how overflow to infinity
  is handled. An exception is raised only when `json.loads` also fails.
  `JSONDecodeError` is re-raised as `fastsimdjson.JSONDecodeError` with the
  same message, document, and position. Any other exception propagates.
