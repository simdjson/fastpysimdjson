import glob
import json
import math
import os
import random
import sys

import pytest

import fastsimdjson

DATA = os.environ.get("JSONDIR", "simdjson-data/jsonexamples")


def same(a, b):
    """Structural equality that also checks types (1 != 1.0, True != 1)."""
    if type(a) is not type(b):
        return False
    if isinstance(a, dict):
        return list(a.keys()) == list(b.keys()) and all(same(a[k], b[k]) for k in a)
    if isinstance(a, list):
        return len(a) == len(b) and all(same(x, y) for x, y in zip(a, b))
    if isinstance(a, float):
        return a == b or (math.isnan(a) and math.isnan(b))
    return a == b


@pytest.mark.parametrize("path", sorted(glob.glob(os.path.join(DATA, "*.json"))))
def test_files(path):
    data = open(path, "rb").read()
    assert same(fastsimdjson.loads(data), json.loads(data))
    assert same(fastsimdjson.loads(data.decode()), json.loads(data))


CASES = [
    "0", "-0", "1", "-1", "123456789", "9223372036854775807", "-9223372036854775808",
    "9223372036854775808", "18446744073709551615", "18446744073709551616",
    "-9223372036854775809", "123456789012345678901234567890",
    "0.0", "-0.0", "1.5", "1e10", "1E-10", "2.2250738585072014e-308", "1.7976931348623157e308",
    "5e-324", "0.1", "3.141592653589793",
    "true", "false", "null", '""', '"a"', "[]", "{}", "[[]]", '{"a":{}}', "[{}]",
    '"\\u0000"', '"\\"\\\\\\/\\b\\f\\n\\r\\t"', '"\\u00e9"', '"é"', '"\\u20ac"', '"€"',
    '"\\ud83d\\ude00"', '"😀"', '"aé€😀"', '"ÿ"', '"Ā"', '"\\u00ff\\u0100"',
    '{"é": 1, "€": 2, "😀": 3}', '{"a": 1, "a": 2}', " [1 , 2 ] ", "\n\t{}\r\n",
    '[1, 2.5, "x", true, false, null, [], {}]',
]


@pytest.mark.parametrize("doc", CASES)
def test_cases(doc):
    assert same(fastsimdjson.loads(doc), json.loads(doc))
    assert same(fastsimdjson.loads(doc.encode()), json.loads(doc))


@pytest.mark.parametrize("n", list(range(0, 200)))
def test_strings_all_lengths(n):
    rnd = random.Random(n)
    alphabets = ["abc", "abcé", "abc€", "abc😀", "é", "€", "😀", 'a"\\\n']
    for alpha in alphabets:
        s = "".join(rnd.choice(alpha) for _ in range(n))
        doc = json.dumps({s: s, "k" + s: [s]}, ensure_ascii=bool(rnd.getrandbits(1)))
        assert same(fastsimdjson.loads(doc), json.loads(doc))


def test_key_cache_collisions():
    keys = [f"key{i}" for i in range(20000)] + ["x" * i for i in range(80)]
    doc = json.dumps([{k: i} for i, k in enumerate(keys)])
    for _ in range(2):
        assert same(fastsimdjson.loads(doc), json.loads(doc))
    # Keys identical in their first and last 8 bytes but different in the middle.
    keys = ["abcdefgh" + chr(65 + i) * 5 + "stuvwxyz" for i in range(26)]
    doc = json.dumps({k: k for k in keys})
    assert same(fastsimdjson.loads(doc), json.loads(doc))


def random_value(rnd, depth=0):
    r = rnd.random()
    if depth > 5 or r < 0.5:
        c = rnd.randrange(6)
        if c == 0:
            return rnd.randint(-(2**63), 2**64 - 1)
        if c == 1:
            return rnd.uniform(-1e10, 1e10)
        if c == 2:
            return "".join(chr(rnd.choice([rnd.randrange(32, 127), rnd.randrange(0x80, 0x800),
                                           rnd.randrange(0x800, 0xD800), rnd.randrange(0x10000, 0x110000)]))
                           for _ in range(rnd.randrange(20)))
        return [True, False, None][c - 3]
    if r < 0.75:
        return [random_value(rnd, depth + 1) for _ in range(rnd.randrange(10))]
    return {f"k{rnd.randrange(50)}": random_value(rnd, depth + 1) for _ in range(rnd.randrange(10))}


@pytest.mark.parametrize("seed", range(50))
def test_random(seed):
    rnd = random.Random(seed)
    v = random_value(rnd)
    doc = json.dumps(v, ensure_ascii=bool(seed % 2))
    assert same(fastsimdjson.loads(doc), json.loads(doc))


def test_input_types():
    doc = b'{"a": [1, 2]}'
    for arg in [doc, bytearray(doc), memoryview(doc), doc.decode()]:
        assert fastsimdjson.loads(arg) == {"a": [1, 2]}
    with pytest.raises(TypeError):
        fastsimdjson.loads(1)


@pytest.mark.parametrize("doc", ["", " ", "[", "]", "{", '{"a"}', '{"a":}', "[1,]", "[1 2]",
                                 '{"a":1,}', "nul", "tru", "01", "1.", ".1", "-", '"abc',
                                 '"\\x"', "[1] x", '"\x01"', b'"\xff"', b'"\xc3"', "NaN", "{1:2}"])
def test_errors(doc):
    with pytest.raises(ValueError):
        fastsimdjson.loads(doc)
    assert issubclass(fastsimdjson.JSONDecodeError, ValueError)


def test_deep_nesting():
    doc = "[" * 1000 + "]" * 1000
    v = fastsimdjson.loads(doc)
    for _ in range(999):
        assert type(v) is list and len(v) == 1
        v = v[0]
    assert v == []
    with pytest.raises(ValueError):
        fastsimdjson.loads("[" * 5000 + "]" * 5000)


def test_page_boundary():
    # Parse many sizes so that some buffers end near a page boundary.
    for n in range(4000, 4200):
        doc = json.dumps(["x" * n]).encode()
        assert fastsimdjson._parse_only(doc) == 0
        assert fastsimdjson.loads(doc) == ["x" * n]


def test_large_array():
    doc = "[" + ",".join(["1"] * (1 << 24 | 5)) + "]"
    v = fastsimdjson.loads(doc)
    assert len(v) == (1 << 24 | 5) and v[0] == 1 and v[-1] == 1


def test_refcounts():
    doc = b'{"key": ["value", 1, 2.5, {"key": null}]}'
    fastsimdjson.loads(doc)
    v = fastsimdjson.loads(doc)
    w = json.loads(doc)
    assert sys.getrefcount(v) == sys.getrefcount(w)
    assert sys.getrefcount(v["key"]) == sys.getrefcount(w["key"])
    assert sys.getrefcount(v["key"][3]) == sys.getrefcount(w["key"][3])


def test_error_type():
    with pytest.raises(json.JSONDecodeError) as info:
        fastsimdjson.loads(b"[1,")
    assert isinstance(info.value, fastsimdjson.JSONDecodeError)
    with pytest.raises(json.JSONDecodeError):
        fastsimdjson.loads("[1,")


def test_big_document_releases_parser():
    doc = json.dumps(["x" * 100] * 800000).encode()  # ~80 MB
    assert len(fastsimdjson.loads(doc)) == 800000
    assert fastsimdjson.loads(b"[1]") == [1]
    # Over the 64 MB retention cap, so the failure path has to drop the parser.
    bad = b"[" + b"x" * 70_000_000
    with pytest.raises(fastsimdjson.JSONDecodeError):
        fastsimdjson.loads(bad)
    assert fastsimdjson.loads(b"[1]") == [1]


def test_bigint_nested_and_memoryview():
    big = "9" * 50
    neg = "-" + "8" * 40
    doc = '{"k": [1, ' + big + ', {"n": ' + neg + "}]} "
    expected = json.loads(doc)
    raw = doc.encode()
    for arg in [doc, raw, bytearray(raw), memoryview(raw)]:
        assert same(fastsimdjson.loads(arg), expected)
    assert fastsimdjson.loads(memoryview(big.encode())) == int(big)


def test_error_position_matches_json():
    docs = [
        "", " ", "[", "[1,", "[1,]", "{", '{"a"}', '{"a":}', "nul", "01",
        '"abc', "[1] x", '{"a":1,}', "\n\n  [1,",
    ]
    for doc in docs:
        with pytest.raises(json.JSONDecodeError) as std:
            json.loads(doc)
        raw = doc.encode()
        for arg in (doc, raw, bytearray(raw), memoryview(raw)):
            with pytest.raises(fastsimdjson.JSONDecodeError) as got:
                fastsimdjson.loads(arg)
            assert got.value.pos == std.value.pos
            assert got.value.lineno == std.value.lineno
            assert got.value.colno == std.value.colno
            assert got.value.msg == std.value.msg
            assert isinstance(got.value, json.JSONDecodeError)


def test_values_json_accepts_stay_rejected():
    # json.loads turns these into floats. The fallback must not return them.
    for doc in ("NaN", "Infinity", "-Infinity", "1e309", "-1e309"):
        json.loads(doc)
        with pytest.raises(fastsimdjson.JSONDecodeError) as got:
            fastsimdjson.loads(doc)
        assert got.value.pos == 0
        with pytest.raises(fastsimdjson.JSONDecodeError) as got:
            fastsimdjson.loads(doc.encode())
        assert got.value.pos == 0


def test_release():
    assert fastsimdjson.release() is None
    assert fastsimdjson.loads(b'{"a": 1}') == {"a": 1}
    assert fastsimdjson.release() is None
    assert fastsimdjson.loads("[1, 2]") == [1, 2]
    assert fastsimdjson.release() is None


def test_release_is_per_thread():
    import threading

    seen = []
    errors = []

    def worker(n):
        try:
            doc = ("[%d]" % n).encode()
            for _ in range(30):
                assert fastsimdjson.loads(doc) == [n]
            assert fastsimdjson.release() is None
            assert fastsimdjson.loads(doc) == [n]
            seen.append(n)
        except Exception as exc:
            errors.append(exc)

    threads = [threading.Thread(target=worker, args=(i,)) for i in range(4)]
    for thread in threads:
        thread.start()
    for thread in threads:
        thread.join()
    assert errors == []
    assert sorted(seen) == [0, 1, 2, 3]


def test_bigint_does_not_accept_surrogates():
    big = "1" * 40
    docs = [
        f'[{big}, "\\ud800"]',
        f'["\\ud800", {big}]',
        f'{{"a": {big}, "b": "\\ud800"}}',
    ]
    for doc in docs:
        raw = doc.encode()
        for arg in [doc, raw, memoryview(raw)]:
            with pytest.raises(fastsimdjson.JSONDecodeError):
                fastsimdjson.loads(arg)
