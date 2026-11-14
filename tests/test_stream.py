import glob
import json
import os
import pathlib
import random

import pytest

import fastsimdjson
from test_lazy import to_py
from test_loads import DATA, random_value, same

FILES = sorted(glob.glob(os.path.join(DATA, "*.json")))


def ndjson(objs, sep="\n"):
    return sep.join(json.dumps(o) for o in objs) + sep


def check_stream(text, expected, **kw):
    got = list(fastsimdjson.loads_many(text, **kw))
    assert len(got) == len(expected)
    assert all(same(a, b) for a, b in zip(got, expected))
    views = list(fastsimdjson.parse_many(text, **kw))
    # The views stay valid after the stream has moved on.
    assert all(same(to_py(a), b) for a, b in zip(views, expected))


def test_corpus_as_ndjson():
    objs = [json.loads(open(f, "rb").read()) for f in FILES]
    if not objs:
        pytest.skip("simdjson-data not available")
    text = ndjson(objs)
    check_stream(text.encode(), objs)
    # Small batches: documents larger than a batch restart the stream.
    check_stream(text.encode(), objs, batch_size=4096)
    check_stream(text, objs, format="lines")


@pytest.mark.parametrize("seed", range(20))
def test_random(seed):
    rng = random.Random(seed)
    objs = [random_value(rng) for _ in range(rng.randrange(1, 40))]
    for sep in ("\n", " ", "\r\n", "\n\n\t"):
        check_stream(ndjson(objs, sep), objs)
    check_stream(ndjson(objs).encode(), objs, batch_size=128)


@pytest.mark.parametrize("text", [b"", b"  \n\t", b"\n\n"])
def test_empty(text):
    assert list(fastsimdjson.loads_many(text)) == []
    assert list(fastsimdjson.parse_many(text)) == []


def test_scalars_and_input_types():
    text = '1 2.5 "x" true false null [] {}'
    expected = [1, 2.5, "x", True, False, None, [], {}]
    raw = text.encode()
    for arg in (text, raw, bytearray(raw), memoryview(raw)):
        assert list(fastsimdjson.loads_many(arg)) == expected


def test_formats():
    objs = [{"a": 1}, [2, 3], "s", 4]
    seq = "".join("\x1e" + json.dumps(o) + "\n" for o in objs)
    check_stream(seq, objs, format="json_seq")
    check_stream(",".join(json.dumps(o) for o in objs), objs, format="comma")
    check_stream(" , ".join(json.dumps(o) for o in objs), objs, format="comma")
    check_stream(json.dumps(objs), objs, format="array")
    check_stream("  " + json.dumps(objs, indent=2) + "\n", objs, format="array")
    check_stream("[]", [], format="array")
    check_stream(ndjson(objs), objs, format="lines")
    check_stream("\ufeff" + ndjson(objs), objs)


def test_json_fallback():
    # Documents that only json accepts are decoded by json, as in loads.
    text = '{"a": 1}\n{"v": 1e400}\n["\\ud800"]\n{"c": 3}\n'
    assert list(fastsimdjson.loads_many(text)) == [{"a": 1}, {"v": float("inf")}, ["\ud800"], {"c": 3}]
    got = list(fastsimdjson.parse_many(text))
    assert to_py(got[0]) == {"a": 1} and got[1] == {"v": float("inf")} and to_py(got[3]) == {"c": 3}


@pytest.mark.parametrize(
    "text",
    ['{"a": 1}\n{"b": }\n{"c": 3}\n', '{"a": 1}\n{"b": [1,', '{"a": 1} x', '[1] [2] {"k" 1}',
     '{"a": "\u00e9\u00e9"}\n{"b": ]}'],
)
def test_errors_match_json(text):
    # The error is json's error for the first bad document, at its position
    # in the whole input.
    pos, expected = 0, []
    decoder = json.JSONDecoder()
    while True:
        while pos < len(text) and text[pos] in " \t\n\r":
            pos += 1
        try:
            obj, pos = decoder.raw_decode(text, pos)
            expected.append(obj)
        except json.JSONDecodeError as e:
            err = e
            break
    got = []
    with pytest.raises(fastsimdjson.JSONDecodeError) as info:
        for x in fastsimdjson.loads_many(text.encode()):
            got.append(x)
    assert got == expected
    assert info.value.msg == err.msg and info.value.pos == err.pos
    assert isinstance(info.value, json.JSONDecodeError)


@pytest.mark.parametrize("fmt", ["json_seq", "comma", "array"])
def test_errors_other_formats(fmt):
    bad = {"json_seq": '\x1e{"a": 1}\n\x1e{"b": ', "comma": '{"a": 1}, {"b": ',
           "array": '[{"a": 1}, {"b": ]'}[fmt]
    with pytest.raises(fastsimdjson.JSONDecodeError):
        list(fastsimdjson.loads_many(bad, format=fmt))


def test_exhausted():
    it = fastsimdjson.loads_many(b"1 2")
    assert list(it) == [1, 2]
    with pytest.raises(StopIteration):
        next(it)
    it = fastsimdjson.loads_many(b"1 {")
    assert next(it) == 1
    with pytest.raises(fastsimdjson.JSONDecodeError):
        next(it)
    with pytest.raises(StopIteration):
        next(it)


def test_bad_arguments():
    with pytest.raises(ValueError):
        fastsimdjson.loads_many(b"1", format="xml")
    with pytest.raises(ValueError):
        fastsimdjson.loads_many(b"1", batch_size=1)
    with pytest.raises(TypeError):
        fastsimdjson.loads_many(1)
    with pytest.raises(TypeError):
        fastsimdjson.loads_many(b"1", "whitespace")


def test_big_documents():
    big = {"k": list(range(200000)), "s": "x" * 100000}
    objs = [big, {"z": 1}, big]
    check_stream(ndjson(objs).encode(), objs, batch_size=1 << 16)


def test_files(tmp_path):
    objs = [{"a": [1, 2]}, "x", None]
    path = tmp_path / "data.ndjson"
    path.write_text(ndjson(objs))
    assert list(fastsimdjson.loads_many(path.read_bytes())) == objs


@pytest.mark.parametrize("fmt", ["whitespace", "lines", "json_seq", "comma", "array"])
def test_big_integers(fmt):
    big = [123456789012345678901234, -(10**40), 2**64]
    objs = [{"n": big[0]}, big, big[2]]
    enc = [json.dumps(o) for o in objs]
    text = {"whitespace": " ".join(enc), "lines": "\n".join(enc) + "\n",
            "json_seq": "".join("\x1e" + e + "\n" for e in enc),
            "comma": ",".join(enc), "array": "[" + ",".join(enc) + "]"}[fmt]
    check_stream(text, objs, format=fmt)


@pytest.mark.parametrize("objs", [[[]], [[], []], [{}, []], [[[]]], [[1], [[2]], []]])
def test_array_of_arrays(objs):
    for sep in (",", " , "):
        check_stream("[" + sep.join(json.dumps(o) for o in objs) + "]", objs, format="array")


@pytest.mark.parametrize("fmt", ["whitespace", "lines"])
def test_byte_order_mark_errors(fmt):
    bom = b"\xef\xbb\xbf"
    assert list(fastsimdjson.loads_many(bom + b"  \n", format=fmt)) == []
    assert list(fastsimdjson.loads_many(bom + b'{"a": 1e400}\n', format=fmt)) == [{"a": float("inf")}]
    for bad in (bom + b'{"a": 1}\n{"b" 2}\n', bom + b"[1,\n", bom + bom + b"[1]\n"):
        text = bad.decode("utf-8")[1:]
        expected = None
        try:
            pos = 0
            dec = json.JSONDecoder()
            while True:
                while pos < len(text) and text[pos] in " \t\n\r":
                    pos += 1
                if pos >= len(text):
                    break
                _, pos = dec.raw_decode(text, pos)
        except json.JSONDecodeError as e:
            expected = (e.msg, e.pos + 1)  # + 1 for the byte order mark
        with pytest.raises(fastsimdjson.JSONDecodeError) as info:
            list(fastsimdjson.loads_many(bad, format=fmt))
        assert (info.value.msg, info.value.pos) == expected


@pytest.mark.parametrize("fmt", ["json_seq", "comma", "array"])
def test_json_fallback_other_formats(fmt):
    # As with white space: documents that only json accepts are decoded by
    # json, and errors are json's, at their position in the whole input.
    docs = ['{"a": 1}', '{"v": 1e400}', '["\\ud800"]', '{"c": 3}']
    expected = [{"a": 1}, {"v": float("inf")}, ["\ud800"], {"c": 3}]
    join = {"json_seq": lambda d: "".join("\x1e" + x + "\n" for x in d),
            "comma": lambda d: ", ".join(d),
            "array": lambda d: "[" + ", ".join(d) + "]"}[fmt]
    assert list(fastsimdjson.loads_many(join(docs), format=fmt)) == expected
    got = [to_py(x) for x in fastsimdjson.parse_many(join(docs), format=fmt)]
    assert got == expected
    bad = join(['{"a": 1}', '{"v": 1e400}', '{"b": ]'])
    with pytest.raises(fastsimdjson.JSONDecodeError) as info:
        list(fastsimdjson.loads_many(bad, format=fmt))
    assert info.value.msg == "Expecting value"
    assert bad[info.value.pos] == "]"


def test_fallback_needs_the_separator():
    # After json decodes a document, the next one must follow a separator.
    with pytest.raises(fastsimdjson.JSONDecodeError) as info:
        list(fastsimdjson.loads_many("1e400 2", format="json_seq"))
    assert info.value.msg == "Expecting record separator"
    with pytest.raises(fastsimdjson.JSONDecodeError) as info:
        list(fastsimdjson.loads_many("[1e400 2]", format="array"))
    assert info.value.msg == "Expecting ',' delimiter"


# threads=N: the list of all documents, as without threads. Inputs over
# 128 KB, so that free-threaded builds use several threads.

def _outcome(fn):
    try:
        return ("ok", fn())
    except Exception as e:  # noqa: BLE001
        return (type(e).__name__, str(e), getattr(e, "pos", None))


def _threaded_input(seed, fmt):
    r = random.Random(seed)
    docs = []
    for i in range(6000):
        c = r.randrange(100)
        if c == 0:
            docs.append(b"[1e400]")  # json only: inf
        elif c == 1:
            docs.append(b'{"big": %d}' % (10**30 + i))
        elif c == 2 and fmt == "whitespace":
            docs.append(b'{\n  "pretty": [1,\n 2]\n}')  # spans lines
        else:
            docs.append(json.dumps({"i": i, "s": "x" * r.randrange(60), "f": i / 7}).encode())
    if seed % 3 == 1:
        docs[r.randrange(len(docs))] = b'{"broken": [1, 2'  # an error somewhere
    if seed % 3 == 2:
        docs[r.randrange(len(docs))] = b'"\\ud800"'  # json only: lone surrogate
    sep = b"\n\x1e" if fmt == "json_seq" else b"\n"
    data = sep.join(docs)
    return b"\x1e" + data if fmt == "json_seq" else data


@pytest.mark.parametrize("fmt", ["whitespace", "lines", "json_seq"])
@pytest.mark.parametrize("seed", range(9))
def test_threads_match_one_thread(fmt, seed):
    data = _threaded_input(seed, fmt)
    expected = _outcome(lambda: list(fastsimdjson.loads_many(data, format=fmt)))
    for threads in (None, 0, 1, 2, 7):
        assert _outcome(lambda: fastsimdjson.loads_many(data, format=fmt, threads=threads)) == expected
    lazy = _outcome(lambda: [to_py(v) for v in fastsimdjson.parse_many(data, format=fmt, threads=4)])
    assert lazy == expected
    mapped = _outcome(lambda: fastsimdjson.map_many(json.dumps, data, format=fmt, threads=4))
    assert mapped == (_outcome(lambda: [json.dumps(d) for d in expected[1]]) if expected[0] == "ok" else expected)


def _framed(parts, fmt):
    sep = b"\n\x1e" if fmt == "json_seq" else b"\n"
    data = sep.join(parts)
    return b"\x1e" + data if fmt == "json_seq" else data


@pytest.mark.parametrize("fmt", ["whitespace", "lines", "json_seq"])
def test_threads_long_record(fmt):
    # A record longer than the 256 KB slice falls across several claims. Those
    # claims are empty and the record is parsed once, with the claim that
    # started before it. An error after it is reported at the same place as
    # one thread, which is the resume point.
    small = [json.dumps({"i": i, "s": "x" * (i % 40)}).encode() for i in range(80)]
    huge = json.dumps({"i": -1, "s": "z" * (1500 * 1024)}).encode()
    tail = [json.dumps({"i": 10000 + i}).encode() for i in range(80)]
    data = _framed(small + [huge] + tail + [huge] + small, fmt)
    expected = list(fastsimdjson.loads_many(data, format=fmt))
    for threads in (2, 4, 7):
        assert fastsimdjson.loads_many(data, format=fmt, threads=threads) == expected
        assert fastsimdjson.loads_many(data, format=fmt, threads=threads,
                                       batch_size=4096) == expected
        assert fastsimdjson.loads_many(data, format=fmt, threads=threads,
                                       fields="/i") == [d["i"] for d in expected]
    broken = _framed(small + [huge] + [b'{"broken": [1'] + tail, fmt)
    assert _outcome(lambda: fastsimdjson.loads_many(broken, format=fmt, threads=4)) == \
        _outcome(lambda: list(fastsimdjson.loads_many(broken, format=fmt)))


@pytest.mark.parametrize("fmt", ["lines", "json_seq"])
@pytest.mark.parametrize("shift", [-2, -1, 0, 1, 2])
def test_threads_delimiter_at_slice_edge(fmt, shift):
    # The delimiter falls on the last byte of a 256 KB slice, on the first
    # byte of the next, or near them; long records span whole slices too.
    slice_size = 256 << 10
    pos = 0 if fmt == "lines" else 1  # the leading record separator
    parts, edge, targets = [], 0, []
    for k in range(1, 9):
        edge += 3 if k % 3 == 0 else 1  # a record longer than two slices
        target = edge * slice_size + shift  # where this record's newline goes
        doc = json.dumps({"k": k, "s": ""}).encode()
        fill = target - pos - len(doc)
        assert fill > 0
        parts.append(doc[:-2] + b"x" * fill + doc[-2:])
        targets.append(target)
        pos = target + (1 if fmt == "lines" else 2)  # past "\n" or "\n\x1e"
    data = _framed(parts, fmt)
    assert [i for i, c in enumerate(data) if c == 0x0A] == targets[:-1]  # none after the last
    expected = list(fastsimdjson.loads_many(data, format=fmt))
    assert [d["k"] for d in expected] == list(range(1, 9))
    for threads in (2, 5):
        assert fastsimdjson.loads_many(data, format=fmt, threads=threads) == expected


def test_threads_api():
    data = b"\n".join(b'{"n": %d}' % i for i in range(30000))
    docs = fastsimdjson.loads_many(data, threads=4)
    assert isinstance(docs, list) and docs == [{"n": i} for i in range(30000)]
    assert fastsimdjson.map_many(lambda d: d["n"], data, threads=3) == list(range(30000))
    assert fastsimdjson.map_many(lambda v: v["n"], data, threads=3, lazy=True) == list(range(30000))
    assert fastsimdjson.map_many(len, b"") == []
    with pytest.raises(ValueError):
        fastsimdjson.loads_many(b"[1],[2]", format="comma", threads=2)
    with pytest.raises(ValueError):
        fastsimdjson.loads_many(data, threads=-1)

    def boom(d):
        if d["n"] == 20000:
            raise KeyError("boom")
        return d

    with pytest.raises(KeyError, match="boom"):
        fastsimdjson.map_many(boom, data, threads=4)


# The worker threads of threads=N persist between calls (free-threaded
# Python): they survive concurrent callers, a fork, and the end of the process.

_POOL_DATA = b"\n".join(b'{"n": %d, "s": "%s"}' % (i, b"x" * (i % 50)) for i in range(40000))


def test_threads_concurrent_callers():
    import threading

    expected = list(range(40000))
    errors = []

    def call():
        for _ in range(5):
            if fastsimdjson.map_many(lambda d: d["n"], _POOL_DATA, threads=8) != expected:
                errors.append("mismatch")

    ts = [threading.Thread(target=call) for _ in range(6)]
    for t in ts:
        t.start()
    for t in ts:
        t.join()
    assert errors == []


@pytest.mark.skipif(not hasattr(os, "fork"), reason="no fork")
def test_threads_after_fork():
    import subprocess
    import sys

    code = (
        "import os, fastsimdjson as F\n"
        "data = b'\\n'.join(b'{\"n\": %d, \"s\": \"%s\"}' % (i, b'x' * (i % 50)) for i in range(6000))\n"
        "n = len(list(F.loads_many(data)))\n"
        "assert len(F.loads_many(data, threads=4)) == n\n"
        "pid = os.fork()\n"
        "if pid == 0:\n"
        "    os._exit(0 if len(F.loads_many(data, threads=4)) == n else 1)\n"
        "_, status = os.waitpid(pid, 0)\n"
        "assert os.waitstatus_to_exitcode(status) == 0\n"
        "print('ok')\n"
    )
    r = subprocess.run([sys.executable, "-W", "ignore", "-c", code], capture_output=True, text=True, timeout=120)
    assert r.returncode == 0 and r.stdout.strip() == "ok", r.stderr


def test_threads_exit_cleanly():
    import subprocess
    import sys

    code = (
        "import fastsimdjson as F, threading\n"
        "data = b'\\n'.join(b'{\"n\": %d, \"s\": \"%s\"}' % (i, b'x' * (i % 50)) for i in range(6000))\n"
        "F.map_many(len, data, threads=8)\n"
        "t = threading.Thread(target=lambda: F.loads_many(data, threads=4), daemon=True)\n"
        "t.start()\n"
        "print('ok')\n"
    )
    r = subprocess.run([sys.executable, "-c", code], capture_output=True, text=True, timeout=120)
    assert r.returncode == 0 and r.stdout.strip() == "ok", r.stderr


def test_map_many_lazy_views_kept():
    # Views that fn keeps (whole or in part, or in an exception) remain valid
    # after map_many, with or without threads.
    data = b"\n".join(json.dumps({"n": i, "a": [i, {"b": str(i)}], "s": "x" * (i % 30)}).encode()
                      for i in range(30000))
    expected = [json.loads(x) for x in data.split(b"\n")]
    for threads in (None, 1, 4):
        kept = fastsimdjson.map_many(lambda v: v, data, threads=threads, lazy=True)
        assert [to_py(v) for v in kept] == expected
        parts = fastsimdjson.map_many(lambda v: v["a"][1], data, threads=threads, lazy=True)
        assert [to_py(v) for v in parts] == [e["a"][1] for e in expected]
        keys = fastsimdjson.map_many(lambda v: list(v.items()), data, threads=threads, lazy=True)
        assert [[(k, to_py(x)) for k, x in kv] for kv in keys] == [list(e.items()) for e in expected]
        some = []
        out = fastsimdjson.map_many(lambda v: some.append(v) if v["n"] % 1000 == 0 else v["n"],
                                    data, threads=threads, lazy=True)
        # With threads, the appends happen in any order.
        assert sorted((to_py(v) for v in some), key=lambda e: e["n"]) == \
            [e for e in expected if e["n"] % 1000 == 0]
        assert out == [None if e["n"] % 1000 == 0 else e["n"] for e in expected]

        def boom(v):
            if v["n"] == 20000:
                raise ValueError(v)  # the exception keeps the view
            return v["n"]

        with pytest.raises(ValueError) as info:
            fastsimdjson.map_many(boom, data, threads=threads, lazy=True)
        assert to_py(info.value.args[0]) == expected[20000]


# fields=: the values at JSON Pointers instead of the documents.

_MISSING = object()


def _pointer_get(doc, pointer, default=None):
    if pointer == "":
        return doc
    for token in pointer[1:].split("/"):
        token = token.replace("~1", "/").replace("~0", "~")
        if isinstance(doc, dict) and token in doc:
            doc = doc[token]
        elif (isinstance(doc, list) and token.isdigit() and (token == "0" or token[0] != "0")
              and int(token) < len(doc)):
            doc = doc[int(token)]
        else:
            return default
    return doc


def _pointers(doc, prefix=""):
    yield prefix
    if isinstance(doc, dict):
        for k, v in doc.items():
            yield from _pointers(v, prefix + "/" + k.replace("~", "~0").replace("/", "~1"))
    elif isinstance(doc, list):
        for i, v in enumerate(doc):
            yield from _pointers(v, f"{prefix}/{i}")


@pytest.mark.parametrize("seed", range(30))
def test_fields_random(seed):
    rnd = random.Random(seed)
    docs = [random_value(rnd) for _ in range(40)]
    docs += [{"a/b": {"~": [1, {"k1": 2}]}, "": 3, "07": 4}, ["x", ["y"]]]
    candidates = sorted({p for d in docs for p in _pointers(d)})
    candidates += ["/nope", "/k1/nope", "/0/0/0", "/00", "/-", "/1x", "/a~1b/~0/1/k1", "/", "/07"]
    text = ndjson(docs).encode()
    for _ in range(10):
        pointers = rnd.sample(candidates, min(len(candidates), rnd.randrange(1, 8)))
        if rnd.random() < 0.5:
            pointers.sort()  # shared prefixes, one after the other
        got = list(fastsimdjson.loads_many(text, fields=pointers))
        assert len(got) == len(docs)
        for g, d in zip(got, docs):
            assert isinstance(g, tuple)
            assert all(same(a, _pointer_get(d, p)) for a, p in zip(g, pointers))
        one = pointers[0]
        got = list(fastsimdjson.loads_many(text, fields=one, default=_MISSING))
        assert all(same(a, _pointer_get(d, one, _MISSING)) for a, d in zip(got, docs))
        assert fastsimdjson.map_many(lambda t: t, text, fields=tuple(pointers)) == \
            list(fastsimdjson.loads_many(text, fields=pointers))


def test_fields_api():
    data = b'{"a": 1, "b": {"c": [10, 20, {"d": "x"}]}}\n{"a": null}\n[5]\n7\n'
    assert list(fastsimdjson.loads_many(data, fields=["/a", "/b/c/2/d", "/b/c/2", "/0"])) == [
        (1, "x", {"d": "x"}, None), (None, None, None, None), (None, None, None, 5),
        (None, None, None, None)]
    # A missing field gets the default; null is a value.
    assert list(fastsimdjson.loads_many(data, fields="/a", default=0)) == [1, None, 0, 0]
    assert list(fastsimdjson.loads_many(data, fields="")) == list(fastsimdjson.loads_many(data))
    assert list(fastsimdjson.loads_many(data, fields=[])) == [(), (), (), ()]
    assert list(fastsimdjson.loads_many(data, fields=None)) == list(fastsimdjson.loads_many(data))
    # Duplicate keys: the first one, as for views (at_pointer).
    assert list(fastsimdjson.loads_many(b'{"a": 1, "a": 2}', fields="/a")) == [1]
    # Documents that only json accepts.
    assert list(fastsimdjson.loads_many(b'{"a": 1e400, "b": [1]}\n{"a": 2}',
                                        fields=["/a", "/b/0"])) == [(float("inf"), 1), (2, None)]
    for bad in ["a", "/~2", "/~"]:
        with pytest.raises(ValueError):
            fastsimdjson.loads_many(data, fields=bad)
    for bad in [1, ["/a", 2], {"/a"}]:
        with pytest.raises(TypeError):
            fastsimdjson.loads_many(data, fields=bad)
    with pytest.raises(ValueError):
        fastsimdjson.map_many(len, data, fields="/a", lazy=True)
    with pytest.raises(TypeError):
        fastsimdjson.loads_many(data, default=1)
    with pytest.raises(TypeError):
        fastsimdjson.parse_many(data, fields="/a")
    with pytest.raises(fastsimdjson.JSONDecodeError):
        list(fastsimdjson.loads_many(b'{"a": 1}\n{"a": ', fields="/a"))


def test_fields_on_demand_validates_what_it_reads():
    # With fields (formats whitespace and lines), only the keys on the way to
    # the pointers and the values found are read: the rest is not validated.
    data = b'{"a": 1, "b": tru, "c": [1,, 2]}\n{"b": {"x": nul}, "a": 2}\n'
    assert list(fastsimdjson.loads_many(data, fields="/a")) == [1, 2]
    assert fastsimdjson.map_many(lambda a: a, data, fields="/a", threads=4) == [1, 2]
    # The whole document is read without fields, or with another format.
    with pytest.raises(fastsimdjson.JSONDecodeError):
        list(fastsimdjson.loads_many(data))
    with pytest.raises(fastsimdjson.JSONDecodeError):
        list(fastsimdjson.loads_many(data.replace(b"\n", b"\x1e"), format="json_seq",
                                     fields="/a"))
    # A value that is read is validated, and a document that only json
    # accepts is decoded by json, as without fields.
    with pytest.raises(fastsimdjson.JSONDecodeError):
        list(fastsimdjson.loads_many(b'{"a": 1}\n{"a": tru}\n', fields="/a"))
    with pytest.raises(fastsimdjson.JSONDecodeError):
        list(fastsimdjson.loads_many(b'{"a": 1}\n{"a": [1,, 2]}\n', fields="/a"))
    big = 10 ** 30
    for text, want in [(b'{"a": %d}' % big, big), (b'{"a": -%d}' % big, -big),
                       (b'{"a": NaN}', "nan"), (b'{"a": 1e400}', float("inf")),
                       (b'{"a": "\\ud800"}', "\ud800"),
                       (b'{"a": [1, {"b": %d}]}' % big, [1, {"b": big}]),
                       (b'{"a": -0}', 0), (b'{"a": -0.0}', -0.0), (b'{"a": 1E2}', 100.0),
                       (b'{"a": 18446744073709551615}', 18446744073709551615),
                       (b'{"a": -9223372036854775808}', -9223372036854775808)]:
        for prefix in (b"", b'{"x": 0}\n'):
            got = list(fastsimdjson.loads_many(prefix + text + b"\n{\"a\": 7}", fields="/a"))
            assert len(got) == 2 + (prefix != b"")
            if want == "nan":
                assert got[-2] != got[-2]
            else:
                assert same(got[-2], want) and type(got[-2]) is type(want)
            assert got[-1] == 7
    # Escaped keys, keys that repeat, values of every kind.
    data = (b'{"a\\u0062": 1, "ab": 2, "c": {"d": [true, false, null, "s\\n", 1.5]},'
            b' "c": 3, "e": {"f": {}, "g": []}}\n')
    assert list(fastsimdjson.loads_many(data, fields=["/ab", "/c/d/3", "/c/d/1", "/c/d/2",
                                                      "/c/d/0", "/c/d/4", "/e/f", "/e/g",
                                                      "/c/d/5"])) == [
        (1, "s\n", False, None, True, 1.5, {}, [], None)]
    # An incomplete last document, and documents that are not objects.
    with pytest.raises(fastsimdjson.JSONDecodeError):
        list(fastsimdjson.loads_many(b'{"a": 1}\n{"a": 2, "b": [', fields="/a"))
    assert list(fastsimdjson.loads_many(b'[1, [2, 3]] "x" 4 {"0": 5}', fields=["/1/1", "/0"])) \
        == [(3, 1), (None, None), (None, None), (None, 5)]


@pytest.mark.parametrize("fmt", ["whitespace", "lines"])
@pytest.mark.parametrize("pointer", ["/a", "/a/b", "/0"])
@pytest.mark.parametrize("text", [
    b'{"a": 1}\n{"a": 2, "b": [',     # incomplete last document
    b'{"a":1,"b":[1}{"a":2}}',         # mismatched brackets: one document, not two
    b'{"a":1,"b":[1}\n{"a":2}}\n',
    b'{"a": 1} {"a": 2}}',             # a stray closing bracket
    b'tru {"a": 1}',                   # an invalid scalar document
    b'{"a": 1} 12x',
    b'{"a": tru}',                     # an invalid value on the pointer's path
    b'{"a": [1, 2,]}',
    b'{"a": 1 "b": 2}',                # separators of the document itself
    b'{"a": 1,}',
    b'[1, 2',
    b'{"a": {"b": 1}, "c": 2',
])
def test_fields_on_demand_document_structure(text, pointer, fmt):
    # The document itself, the scalars and the steps that a pointer reaches
    # are checked: the outcome is that of the whole documents.
    def outcome(fields):
        try:
            return "ok", list(fastsimdjson.loads_many(text, format=fmt, fields=fields))
        except fastsimdjson.JSONDecodeError as e:
            return "error", e.pos
    if (text, pointer) in ((b'{"a": tru}', "/0"), (b'{"a": [1, 2,]}', "/0"),
                           (b'{"a": [1, 2,]}', "/a/b")):
        # An invalid value that no pointer reaches is not checked (/a/b
        # cannot be in an array: its elements are not read).
        assert outcome(pointer) == ("ok", [None])
        return
    expected = outcome(None)
    got = outcome(pointer)
    if expected[0] == "ok":
        assert got == ("ok", [_pointer_get(d, pointer) for d in expected[1]])
    else:
        assert got[0] == "error"
    # Iterating by hand: the same values, up to the same error (no value is
    # returned for a document that turns out to be invalid).
    def until_error(fields):
        seen = []
        try:
            for v in fastsimdjson.loads_many(text, format=fmt, fields=fields):
                seen.append(v)
        except fastsimdjson.JSONDecodeError:
            pass
        return seen
    assert until_error(pointer) == [_pointer_get(d, pointer) for d in until_error(None)]


@pytest.mark.parametrize("seed", range(10))
def test_fields_on_demand_matches_whole_documents(seed):
    # Pointers none of which leads into another (the On-Demand walk), on
    # random documents, with and without threads.
    rnd = random.Random(seed)
    docs = [random_value(rnd) for _ in range(300)]
    candidates = sorted({p for d in docs for p in _pointers(d) if p})
    text = ndjson(docs).encode()
    for _ in range(10):
        pointers = []
        for p in rnd.sample(candidates, min(len(candidates), 12)):
            if all(not (p + "/").startswith(q + "/") and not (q + "/").startswith(p + "/")
                   for q in pointers):
                pointers.append(p)
        want = [tuple(_pointer_get(d, p) for p in pointers) for d in docs]
        for threads in (None, 4):
            got = fastsimdjson.map_many(lambda t: t, text, fields=pointers, threads=threads)
            assert len(got) == len(want)
            assert all(all(same(a, b) for a, b in zip(g, w)) for g, w in zip(got, want))


@pytest.mark.parametrize("fmt", ["whitespace", "lines", "json_seq"])
@pytest.mark.parametrize("seed", range(6))
def test_fields_threads(fmt, seed):
    data = _threaded_input(seed, fmt)
    pointers = ["/i", "/s", "/big", "/0", "/pretty/1", "/f"]
    expected = _outcome(lambda: list(fastsimdjson.loads_many(data, format=fmt, fields=pointers)))
    if expected[0] == "ok":
        docs = list(fastsimdjson.loads_many(data, format=fmt))
        assert expected[1] == [tuple(_pointer_get(d, p) for p in pointers) for d in docs]
    for threads in (None, 1, 4):
        assert _outcome(lambda: fastsimdjson.loads_many(data, format=fmt, fields=pointers,
                                                        threads=threads)) == expected
        assert _outcome(lambda: fastsimdjson.map_many(lambda t: t, data, format=fmt,
                                                      fields=pointers, threads=threads)) == expected


# release() also frees the parsers and caches of the pool's workers (on a
# free-threaded build). These check that the streams keep working after it,
# that it does not wait for itself when called from fn, and that it can run
# at the same time as threaded streams.
def _pool_input():
    objs = [{"i": i, "name": "n%d" % i, "tags": ["a", "b", i % 7]} for i in range(60000)]
    return ndjson(objs).encode(), objs


def _no_hang(target, timeout=120):
    import threading

    errors = []

    def run():
        try:
            target()
        except BaseException as exc:  # reported below
            errors.append(exc)

    thread = threading.Thread(target=run)
    thread.start()
    thread.join(timeout)
    assert not thread.is_alive(), "deadlock"
    if errors:
        raise errors[0]


def test_release_after_threads():
    data, objs = _pool_input()
    assert fastsimdjson.loads_many(data, threads=4) == objs
    assert fastsimdjson.release() is None
    assert fastsimdjson.loads_many(data, threads=4) == objs
    assert [to_py(v) for v in fastsimdjson.parse_many(data, threads=4)] == objs
    assert fastsimdjson.release() is None
    assert fastsimdjson.map_many(len, data, threads=8) == [len(o) for o in objs]


def test_release_inside_map_many():
    data, objs = _pool_input()

    def fn(doc):
        if doc["i"] % 5000 == 0:
            fastsimdjson.release()
        return doc["i"]

    def body():
        assert fastsimdjson.map_many(fn, data, threads=4) == [o["i"] for o in objs]

    _no_hang(body)


def test_release_during_threaded_runs():
    import threading

    data, objs = _pool_input()
    stop = threading.Event()

    def releaser():
        while not stop.is_set():
            fastsimdjson.release()

    def body():
        thread = threading.Thread(target=releaser)
        thread.start()
        try:
            for _ in range(5):
                assert fastsimdjson.loads_many(data, threads=4) == objs
        finally:
            stop.set()
            thread.join()

    _no_hang(body)


def test_long_record_then_threads():
    # A record past 64 MB grows the parser of the worker that reads it; the
    # parser is freed after the run, and the next runs get a new one.
    data, objs = _pool_input()
    big = {"big": "x" * (65 << 20)}
    text = data + json.dumps(big).encode() + b"\n" + data
    out = fastsimdjson.loads_many(text, threads=4)
    assert len(out) == 2 * len(objs) + 1 and out[len(objs)] == big
    del out
    assert fastsimdjson.loads_many(data, threads=4) == objs


def test_readonly_memoryview_in_place(tmp_path):
    import mmap
    objs = [{"id": i, "name": f"n{i}", "tags": ["a", "b"][: i % 3]} for i in range(20000)]
    data = ndjson(objs).encode()
    path = tmp_path / "events.ndjson"
    path.write_bytes(data)
    pick = lambda d: d["id"]
    want = [o["id"] for o in objs]
    with open(path, "rb") as f, mmap.mmap(f.fileno(), 0, access=mmap.ACCESS_READ) as m:
        view = memoryview(m)
        assert view.readonly
        assert list(fastsimdjson.loads_many(view)) == objs
        assert [to_py(v) for v in fastsimdjson.parse_many(view)] == objs
        assert fastsimdjson.map_many(pick, view, threads=4) == want
        assert fastsimdjson.map_many(pick, view, threads=4, format="lines") == want
        assert list(fastsimdjson.loads_many(view, fields="/name")) == [o["name"] for o in objs]
        # The stream keeps the mapping exported: the caller may release its
        # view, and the mmap cannot be closed under the stream.
        it = fastsimdjson.loads_many(view)
        assert next(it) == objs[0]
        view.release()
        with pytest.raises(BufferError):
            m.close()
        assert list(it) == objs[1:]
        del it
    # A read-only view of bytes, sliced, is read in place too.
    part = memoryview(data)[: len(ndjson(objs[:10]))]
    assert list(fastsimdjson.loads_many(part)) == objs[:10]


def test_mutable_exporters_are_copied(tmp_path):
    # A read-only view of something writable is copied: the exporter can
    # still change while the stream reads it.
    import mmap
    objs = [{"x": i} for i in range(100)]
    text = ndjson(objs).encode()
    path = tmp_path / "x.ndjson"
    path.write_bytes(text)
    ba = bytearray(text)
    with open(path, "r+b") as f, mmap.mmap(f.fileno(), 0) as writable:
        for owner, view in ((ba, memoryview(ba).toreadonly()),
                            (writable, memoryview(writable).toreadonly())):
            it = fastsimdjson.loads_many(view)
            assert next(it) == objs[0]
            owner[:] = bytes(owner).replace(b'"x"', b'"y"')   # same length
            assert list(it) == objs[1:]
            del it, view
    # bytes, through any number of read-only views, is read in place.
    first = len(ndjson(objs[:1]))
    view = memoryview(memoryview(text)[1:]).toreadonly()
    assert list(fastsimdjson.loads_many(view[first - 1:])) == objs[1:]


def test_writable_buffers_are_copied():
    objs = [{"x": i} for i in range(100)]
    for view in (False, True):
        data = bytearray(ndjson(objs).encode())
        it = fastsimdjson.loads_many(memoryview(data) if view else data)
        assert next(it) == objs[0]
        data[:] = data.replace(b'"x"', b'"y"')   # same length
        assert list(it) == objs[1:]
