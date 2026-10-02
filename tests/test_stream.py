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
