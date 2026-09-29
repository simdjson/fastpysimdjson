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

## How it works

1. simdjson's DOM parser (with runtime CPU dispatch: AVX-512, AVX2, SSE4.2, ...)
   validates the document and builds its tape. The parser is reused across
   calls. Input is copied only when the missing padding would cross a page
   boundary.
2. A tape walker creates the Python objects directly:
   * lists are allocated at their final size (simdjson records element counts);
     scalars are handled inline in the array/object loops;
   * dicts are presized and filled with `_PyDict_SetItem_KnownHash`;
   * object keys go through a direct-mapped cache (ASCII keys of up to 64
     bytes), so repeated keys reuse one `str` object whose hash is already
     computed. Short ASCII string values (up to 16 bytes) have their own cache;
   * strings are built with `PyUnicode_New` plus a copy. Non-ASCII UTF-8,
     which simdjson has already validated, is transcoded without
     revalidation: scalar code for short strings, simdutf for long ones;
   * the cyclic GC is paused while objects are built.
3. Integers that do not fit in 64 bits stay on the tape as digit strings and
   become exact Python ints (orjson turns them into floats).

Like orjson, parsing is strict: `NaN`/`Infinity`, lone surrogate escapes
(`"\ud800"`), and numbers that overflow a double are rejected. Unlike orjson,
a leading UTF-8 BOM is accepted.

## Build and test

Python 3.10 or newer, and a C++17 compiler (clang or GCC). The simdjson 5.0.1
and simdutf 9.2.1 amalgamations are already in `vendor/`.

```sh
python3 -m venv .venv
. .venv/bin/activate
python -m pip install -U pip setuptools
python -m pip install -e ".[test]"
pytest tests
```

That builds the extension and makes `import fastsimdjson` work in the
virtualenv. To compile it in the tree instead:

```sh
python -m pip install setuptools pytest
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

To time `loads` against orjson on those files:

```sh
python -m pip install -e ".[bench]"
python bench.py
```

## Benchmarks

`loads(bytes)` on Intel Xeon Gold 6548N (Emerald Rapids), Python 3.12.13,
GCC 14, orjson 3.12.0. Times are the best of several runs. Speedup is
orjson time / fastsimdjson time.

| file | KB | orjson µs | fastsimdjson µs | speedup |
|---|---:|---:|---:|---:|
| apache_builds.json | 124 | 182.1 | 153.0 | 1.19 |
| canada.json | 2198 | 7133.4 | 5619.5 | 1.27 |
| citm_catalog.json | 1687 | 2833.7 | 1777.7 | 1.59 |
| github_events.json | 64 | 69.3 | 55.4 | 1.25 |
| google_maps_api_compact_response.json | 12 | 39.7 | 32.9 | 1.21 |
| google_maps_api_response.json | 25 | 44.0 | 34.7 | 1.27 |
| gsoc-2018.json | 3250 | 3831.6 | 2234.3 | 1.71 |
| instruments.json | 215 | 317.3 | 232.7 | 1.36 |
| marine_ik.json | 2914 | 10655.6 | 7915.5 | 1.35 |
| mesh.json | 707 | 1673.6 | 1464.8 | 1.14 |
| mesh.pretty.json | 1540 | 2291.9 | 1638.4 | 1.40 |
| numbers.json | 147 | 220.3 | 229.7 | 0.96 |
| random.json | 499 | 1499.0 | 1297.9 | 1.15 |
| repeat.json | 11 | 15.4 | 13.0 | 1.18 |
| semanticscholar-corpus.json | 8392 | 23044.4 | 15055.0 | 1.53 |
| tree-pretty.json | 34 | 42.4 | 35.8 | 1.19 |
| twitter.json | 617 | 1008.3 | 721.4 | 1.40 |
| twitter_api_compact_response.json | 10 | 14.5 | 12.1 | 1.20 |
| twitter_api_response.json | 15 | 17.1 | 14.3 | 1.19 |
| twitter_timeline.json | 41 | 60.8 | 52.4 | 1.16 |
| twitterescaped.json | 549 | 1024.4 | 801.4 | 1.28 |
| update-center.json | 521 | 1353.3 | 1114.5 | 1.21 |

On Python 3.14 the results are similar (1.13×–1.86×; numbers.json 0.99×).
numbers.json, a flat array of floats, is limited by simdjson's float parsing
(about 10 ns per number here) plus CPython's float allocation, which orjson
pays too.

## Limitations

* Only `loads` is implemented; there is no `dumps`.
* A global parser and caches are used under the GIL. The module does not
  declare free-threading support, so free-threaded builds re-enable the GIL
  when it is imported.
* Error messages come from simdjson and carry no position (`pos` is 0).
