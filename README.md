# fastsimdjson

A Python binding for [simdjson](https://github.com/simdjson/simdjson) that
parses JSON into native Python objects (`dict`, `list`, `str`, `int`, `float`,
`bool`, `None`). It is intended as a faster drop-in for `orjson.loads` /
`json.loads`.

```python
import fastsimdjson
fastsimdjson.loads(b'{"a": [1, 2.5, "x", true, null]}')
```

`loads` accepts `bytes`, `bytearray`, `memoryview` and `str`. Invalid input
raises `fastsimdjson.JSONDecodeError`, a subclass of `json.JSONDecodeError`.
When simdjson rejects a document, the same input is parsed with `json.loads`.
If that succeeds, `loads` returns its value. If it raises `JSONDecodeError`,
the exception is re-raised with Python's message and byte position. Any other
exception from `json.loads` propagates. `release()` frees the simdjson parser
and the string caches kept by the calling thread.

## How it works

1. simdjson's DOM parser (with runtime CPU dispatch: AVX-512, AVX2, SSE4.2, ...)
   validates the document and builds its tape. Each thread keeps its own
   parser and reuses it across calls; `release()` deletes that parser and
   drops the thread's cached strings. The document is parsed in place. When
   the 64 bytes simdjson would read past the end cross a page boundary, the
   unpadded DOM parser is used instead of copying the buffer.
2. A tape walker creates the Python objects directly:
   * lists are allocated at their final size (simdjson records element counts);
     scalars are handled inline in the array/object loops;
   * dicts are presized and filled with `_PyDict_SetItem_KnownHash`;
   * object keys go through a direct-mapped cache (ASCII keys of up to 64
     bytes), so repeated keys reuse one `str` object whose hash is already
     computed. Short ASCII string values (up to 16 bytes) have their own cache.
     Both caches belong to the calling thread;
   * strings are built with `PyUnicode_New` plus a copy. Non-ASCII UTF-8,
     which simdjson has already validated, is transcoded without
     revalidation: scalar code for short strings, simdutf for long ones;
   * on builds that use the GIL, the cyclic GC is paused while objects are built.
3. Integers that do not fit in 64 bits stay on the tape as digit strings and
   become exact Python ints (orjson turns them into floats).

`NaN`, `Infinity`, and `-Infinity` parse as floats, matching `json.loads`.
simdjson is built with `SIMDJSON_ENABLE_NAN_INF`, so any capitalization of
`nan`, `inf`, and `infinity` is also accepted (Python's parser only allows
the three spellings above). A number that overflows a double becomes
`inf`, via `json.loads`, because simdjson still rejects it. An unpaired
surrogate escape (`"\ud800"`) is also rejected by simdjson; `json.loads`
accepts it, so `loads` returns that string. A leading UTF-8 BOM is accepted.

## Build and test

Python 3.10 or newer, and a C++17 compiler (clang or GCC). The simdjson 5.0.1
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
order. It covers scalars, integers past 64 bits, UTF-8 strings at every
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

To time `loads` against `json.loads` and orjson on those files:

```sh
python -m pip install -e ".[bench]"
python bench.py
```

```sh
uv pip install -e ".[bench]"
python bench.py
```

## Benchmarks

Full materialization of `simdjson-data` on an Intel Xeon Gold 6548N, pinned
to one core. Python 3.12.13, GCC, `json.loads`, orjson 3.12.0, yyjson 4.0.6,
cysimdjson 26.27 (simdjson 3.8.0, AVX-512), pysimdjson 7.0.2 (icelake,
AVX-512). Times are microseconds, the best of several runs. Speedup is
orjson time / fastsimdjson time.

cysimdjson is `JSONParser.parse(data).export()` and pysimdjson is
`Parser.parse(data, recursive=True)`. `yyjson.loads` is `Document.as_obj`.
fastsimdjson, `json.loads`, cysimdjson, and pysimdjson matched orjson on
every file, including types and key order.

Geomean speedup of fastsimdjson: 3.38× over `json.loads`, 1.27× over orjson,
1.39× over yyjson, 1.83× over cysimdjson, and 1.79× over pysimdjson. It was
the fastest on 21 files. `numbers.json`, a flat array of floats, is the loss
against orjson (0.95×): the time is simdjson's float parser plus allocating
Python floats. `json.loads` is slowest on every file; the gap is largest on
`canada.json` (about 5.8×).

yyjson 4.0.6 returns non-ASCII strings as the raw UTF-8 bytes stored in a
Latin-1 `str`. Twelve files therefore do not match orjson (the twitter
files, `citm_catalog`, `gsoc-2018`, `github_events`, `random`, `repeat`,
`semanticscholar-corpus`, and `update-center`). Those yyjson times skip a
real UTF-8 decode. fastsimdjson is still ahead on the ten files where the
values match.

| file | KB | json | orjson | fast | yyjson | cys | psy | speedup |
|---|---:|---:|---:|---:|---:|---:|---:|---:|
| apache_builds.json | 124 | 383.2 | 180.7 | 153.9 | 205.9 | 263.1 | 264.8 | 1.17 |
| canada.json | 2198 | 30404.4 | 6518.2 | 5248.1 | 5552.2 | 7894.4 | 7348.4 | 1.24 |
| citm_catalog.json | 1687 | 7295.1 | 2934.6 | 1775.7 | 2554.0 | 4321.0 | 4206.3 | 1.65 |
| github_events.json | 64 | 216.1 | 68.5 | 55.6 | 82.8 | 98.5 | 97.7 | 1.23 |
| google_maps_api_compact_response.json | 12 | 80.3 | 39.7 | 33.6 | 47.3 | 55.6 | 56.5 | 1.18 |
| google_maps_api_response.json | 25 | 93.6 | 43.4 | 34.8 | 52.1 | 56.9 | 57.7 | 1.25 |
| gsoc-2018.json | 3250 | 7039.9 | 3889.8 | 2280.5 | 2968.0 | 3569.8 | 3611.0 | 1.71 |
| instruments.json | 215 | 848.4 | 319.3 | 239.1 | 405.1 | 478.6 | 500.7 | 1.34 |
| marine_ik.json | 2914 | 23662.6 | 9301.2 | 7272.7 | 8548.5 | 12315.3 | 11046.9 | 1.28 |
| mesh.json | 707 | 5093.0 | 1712.2 | 1493.1 | 1685.6 | 2269.3 | 2074.4 | 1.15 |
| mesh.pretty.json | 1540 | 8382.9 | 2435.5 | 1674.3 | 2185.0 | 2548.5 | 2309.9 | 1.45 |
| numbers.json | 147 | 914.3 | 221.4 | 232.3 | 260.4 | 299.2 | 276.2 | 0.95 |
| random.json | 499 | 3107.1 | 1522.5 | 1292.4 | 1632.5 | 2571.5 | 2390.8 | 1.18 |
| repeat.json | 11 | 43.9 | 15.3 | 13.0 | 16.7 | 30.5 | 24.1 | 1.18 |
| semanticscholar-corpus.json | 8392 | 45587.1 | 20634.5 | 14113.1 | 18832.2 | 28079.0 | 26477.3 | 1.46 |
| tree-pretty.json | 34 | 117.5 | 43.5 | 36.3 | 62.5 | 63.5 | 65.4 | 1.20 |
| twitter.json | 617 | 2798.4 | 1017.4 | 715.4 | 1168.0 | 1771.2 | 1825.1 | 1.42 |
| twitter_api_compact_response.json | 10 | 45.0 | 14.8 | 12.1 | 19.3 | 22.6 | 22.7 | 1.22 |
| twitter_api_response.json | 15 | 60.3 | 17.4 | 13.9 | 23.4 | 26.2 | 26.4 | 1.25 |
| twitter_timeline.json | 41 | 176.7 | 61.2 | 52.6 | 82.8 | 110.9 | 114.6 | 1.16 |
| twitterescaped.json | 549 | 2308.5 | 1021.7 | 801.4 | 1184.0 | 1961.0 | 2030.3 | 1.27 |
| update-center.json | 521 | 2675.8 | 1363.6 | 1119.1 | 1509.2 | 1982.1 | 2057.0 | 1.22 |

## Limitations

* Only `loads` is implemented; there is no `dumps`.
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
