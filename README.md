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
When simdjson rejects a document, the same input is parsed with `json.loads`
so the exception carries Python's message and byte position. `release()`
frees the simdjson parser kept by the calling thread.

## How it works

1. simdjson's DOM parser (with runtime CPU dispatch: AVX-512, AVX2, SSE4.2, ...)
   validates the document and builds its tape. Each thread keeps its own
   parser and reuses it across calls; `release()` deletes that parser.
   Input is copied only when the missing padding would cross a page
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

To time `loads` against orjson on those files:

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
to one core. Python 3.12.13, GCC, orjson 3.12.0, yyjson 4.0.6, cysimdjson
26.27 (simdjson 3.8.0, AVX-512), pysimdjson 7.0.2 (icelake, AVX-512). Times
are microseconds, the best of several runs. Speedup is orjson time /
fastsimdjson time.

cysimdjson is `JSONParser.parse(data).export()` and pysimdjson is
`Parser.parse(data, recursive=True)`. `yyjson.loads` is `Document.as_obj`.
fastsimdjson, cysimdjson, and pysimdjson matched orjson on every file,
including types and key order.

Geomean speedup of fastsimdjson: 1.28× over orjson, 1.41× over yyjson, 1.83×
over cysimdjson, and 1.83× over pysimdjson. It was the fastest on 21 files.
`numbers.json`, a flat array of floats, is the loss against orjson (0.99×):
the time is simdjson's float parser plus allocating Python floats.

yyjson 4.0.6 returns non-ASCII strings as the raw UTF-8 bytes stored in a
Latin-1 `str`. Twelve files therefore do not match orjson (the twitter
files, `citm_catalog`, `gsoc-2018`, `github_events`, `random`, `repeat`,
`semanticscholar-corpus`, and `update-center`). Those yyjson times skip a
real UTF-8 decode. fastsimdjson is still ahead on the ten files where the
values match.

| file | KB | orjson | fast | yyjson | cys | psy | speedup |
|---|---:|---:|---:|---:|---:|---:|---:|
| apache_builds.json | 124 | 180.8 | 148.9 | 207.1 | 262.2 | 268.1 | 1.21 |
| canada.json | 2198 | 6539.6 | 5294.2 | 5728.7 | 7833.3 | 7363.5 | 1.24 |
| citm_catalog.json | 1687 | 2915.1 | 1772.2 | 2569.4 | 4308.2 | 4215.6 | 1.64 |
| github_events.json | 64 | 68.7 | 54.1 | 83.5 | 109.1 | 105.1 | 1.27 |
| google_maps_api_compact_response.json | 12 | 40.3 | 34.1 | 48.0 | 56.9 | 57.2 | 1.18 |
| google_maps_api_response.json | 25 | 43.7 | 35.0 | 52.7 | 57.7 | 58.5 | 1.25 |
| gsoc-2018.json | 3250 | 3887.6 | 2279.7 | 2983.5 | 3538.0 | 3613.0 | 1.71 |
| instruments.json | 215 | 323.0 | 237.5 | 410.0 | 502.8 | 504.2 | 1.36 |
| marine_ik.json | 2914 | 9593.7 | 6680.2 | 8308.6 | 11567.4 | 11210.5 | 1.44 |
| mesh.json | 707 | 1709.2 | 1473.8 | 1680.5 | 2275.3 | 2291.4 | 1.16 |
| mesh.pretty.json | 1540 | 2438.1 | 1651.8 | 2187.1 | 2550.9 | 2530.8 | 1.48 |
| numbers.json | 147 | 222.7 | 224.3 | 259.2 | 299.8 | 276.2 | 0.99 |
| random.json | 499 | 1548.7 | 1290.7 | 1688.1 | 2435.7 | 2459.8 | 1.20 |
| repeat.json | 11 | 15.2 | 12.9 | 16.9 | 24.3 | 24.6 | 1.18 |
| semanticscholar-corpus.json | 8392 | 19783.5 | 13340.7 | 18140.7 | 26555.3 | 26659.4 | 1.48 |
| tree-pretty.json | 34 | 42.7 | 35.2 | 61.3 | 62.3 | 65.3 | 1.21 |
| twitter.json | 617 | 1012.8 | 727.1 | 1183.8 | 1789.8 | 1838.3 | 1.39 |
| twitter_api_compact_response.json | 10 | 14.7 | 12.1 | 19.5 | 22.4 | 22.6 | 1.21 |
| twitter_api_response.json | 15 | 17.6 | 14.3 | 23.9 | 26.5 | 26.6 | 1.23 |
| twitter_timeline.json | 41 | 60.7 | 51.9 | 82.4 | 112.9 | 112.6 | 1.17 |
| twitterescaped.json | 549 | 1016.6 | 839.6 | 1191.0 | 2003.6 | 2049.4 | 1.21 |
| update-center.json | 521 | 1395.4 | 1134.2 | 1536.4 | 2030.1 | 2062.9 | 1.23 |

## Limitations

* Only `loads` is implemented; there is no `dumps`.
* The simdjson parser is thread-local. `release()` frees the one retained by
  the calling thread. A parser that grows past 64 MB is freed on its own at
  the end of that call. Key and short-string caches are process-global and
  updated under the GIL. The module does not declare free-threading support,
  so free-threaded builds re-enable the GIL when it is imported.
* `json.loads` accepts some documents simdjson rejects (`NaN`, `Infinity`,
  and floats that overflow to infinity). Those still raise
  `JSONDecodeError`, with `pos` 0, because there is no Python error to copy.
  The same `pos` 0 fallback is used when `json.loads` raises something other
  than `JSONDecodeError`.
