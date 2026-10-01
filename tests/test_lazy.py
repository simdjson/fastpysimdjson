import collections.abc
import gc
import glob
import json
import os
import random
import threading

import pytest

import fastsimdjson
from test_loads import DATA, random_value, same


def to_py(v):
    """Converts a lazy value by walking it through the view API only."""
    if isinstance(v, fastsimdjson.Object):
        return {k: to_py(x) for k, x in v.items()}
    if isinstance(v, fastsimdjson.Array):
        return [to_py(x) for x in v]
    return v


def check(text):
    expected = json.loads(text)
    lazy = fastsimdjson.parse(text)
    assert same(to_py(lazy), expected)
    if isinstance(lazy, fastsimdjson.Object):
        assert same(lazy.as_dict(), expected)
        assert len(lazy) == len(expected)
        assert list(lazy) == list(expected)
        assert list(lazy.keys()) == list(expected)
        for k in expected:
            assert k in lazy
            assert same(to_py(lazy[k]), expected[k])
            assert same(to_py(lazy.get(k)), expected[k])
    elif isinstance(lazy, fastsimdjson.Array):
        assert same(lazy.as_list(), expected)
        assert len(lazy) == len(expected)
        for i in range(len(expected)):
            assert same(to_py(lazy[i]), expected[i])
            assert same(to_py(lazy[i - len(expected)]), expected[i])


@pytest.mark.parametrize("path", sorted(glob.glob(os.path.join(DATA, "*.json"))))
def test_files(path):
    check(open(path, "rb").read())


@pytest.mark.parametrize("seed", range(50))
def test_random(seed):
    rng = random.Random(seed)
    check(json.dumps(random_value(rng), ensure_ascii=bool(seed % 2)))


@pytest.mark.parametrize(
    "doc",
    ['{}', '[]', '{"a": {}}', '[[]]', '"x"', '1', '-1.5', 'true', 'null',
     '18446744073709551615', '123456789012345678901234567890',
     '{"a": [1, 2.5, "x", true, false, null, {"b": []}]}'],
)
def test_cases(doc):
    check(doc)


def test_scalar_root_is_plain():
    assert fastsimdjson.parse(b"42") == 42
    assert fastsimdjson.parse(b'"hi"') == "hi"
    assert fastsimdjson.parse(b"null") is None


def test_types_and_abc():
    d = fastsimdjson.parse(b'{"a": [1]}')
    assert type(d) is fastsimdjson.Object
    assert type(d["a"]) is fastsimdjson.Array
    assert isinstance(d, collections.abc.Mapping)
    assert isinstance(d["a"], collections.abc.Sequence)
    assert repr(d) == "<fastsimdjson.Object with 1 keys>"
    assert repr(d["a"]) == "<fastsimdjson.Array with 1 elements>"
    with pytest.raises(TypeError):
        fastsimdjson.Object()
    with pytest.raises(TypeError):
        fastsimdjson.Array()


def test_match_statement():
    d = fastsimdjson.parse(b'{"kind": "point", "xy": [1, 2]}')
    match d:
        case {"kind": "point", "xy": [x, y]}:
            assert (x, y) == (1, 2)
        case _:
            pytest.fail("no match")


def test_missing_keys_and_indexes():
    d = fastsimdjson.parse(b'{"a": [1, 2, 3]}')
    with pytest.raises(KeyError):
        d["b"]
    with pytest.raises(KeyError):
        d[0]
    assert 0 not in d and "b" not in d
    assert d.get("b") is None and d.get("b", 7) == 7
    assert "\ud800" not in d
    a = d["a"]
    for k in (3, -4, 10**6):
        with pytest.raises(IndexError):
            a[k]
    with pytest.raises(TypeError):
        a["x"]
    assert a[True] == 2


def test_slices():
    expected = list(range(20))
    a = fastsimdjson.parse(json.dumps(expected))
    for s in (slice(None), slice(3, 9), slice(None, None, -1), slice(1, 19, 3),
              slice(-5, None), slice(15, 2, -4), slice(30, 40)):
        assert a[s] == expected[s]


def test_sequential_and_random_indexing():
    expected = [{"i": i} if i % 3 else [i] for i in range(500)]
    a = fastsimdjson.parse(json.dumps(expected))
    assert [to_py(a[i]) for i in range(len(a))] == expected
    rng = random.Random(1)
    for _ in range(500):
        i = rng.randrange(-500, 500)
        assert to_py(a[i]) == expected[i]


def test_duplicate_keys():
    # Lookups return the first value; as_dict, like json.loads, keeps the last.
    d = fastsimdjson.parse(b'{"a": 1, "a": 2}')
    assert d["a"] == 1
    assert len(d) == 2 and list(d) == ["a", "a"]
    assert d.as_dict() == json.loads('{"a": 1, "a": 2}') == {"a": 2}


def test_at_pointer():
    doc = {"a": [10, {"b/c": 1, "d~e": 2, "": 3}], "f": {"0": "zero"}}
    d = fastsimdjson.parse(json.dumps(doc))
    assert to_py(d.at_pointer("")) == doc
    assert d.at_pointer("/a/0") == 10
    assert d.at_pointer("/a/1/b~1c") == 1
    assert d.at_pointer("/a/1/d~0e") == 2
    assert d.at_pointer("/a/1/") == 3
    assert d.at_pointer("/f/0") == "zero"
    assert d["a"].at_pointer("/1/b~1c") == 1
    assert to_py(d["a"].at_pointer("")) == doc["a"]
    with pytest.raises(KeyError):
        d.at_pointer("/zz")
    for bad in ("/a/2", "/a/-", "/a/01", "/a/x", "/a/"):
        with pytest.raises(IndexError):
            d.at_pointer(bad)
    for bad in ("a", "/a/0/x", "/a/1/~2"):
        with pytest.raises(ValueError):
            d.at_pointer(bad)
    with pytest.raises(TypeError):
        d.at_pointer(1)


def test_document_outlives_parser_reuse():
    docs = [json.dumps({"n": n, "s": "x" * n, "l": list(range(n))}) for n in range(50)]
    views = [fastsimdjson.parse(t) for t in docs]
    # Interleave loads and parse calls that reuse the thread's parser.
    for t in reversed(docs):
        fastsimdjson.loads(t)
        fastsimdjson.parse(t)
    for v, t in zip(views, docs):
        assert to_py(v) == json.loads(t)


def test_views_keep_document_alive():
    d = fastsimdjson.parse(b'{"a": {"b": [1, 2, {"c": "deep"}]}}')
    inner = d["a"]["b"]
    it = iter(inner)
    del d
    gc.collect()
    for _ in range(10):
        fastsimdjson.parse(b'{"other": [0, 0, 0, 0, 0, 0, 0, 0]}')
    assert inner[2]["c"] == "deep"
    assert next(it) == 1


def test_errors():
    with pytest.raises(fastsimdjson.JSONDecodeError):
        fastsimdjson.parse(b'{"a": ')
    with pytest.raises(TypeError):
        fastsimdjson.parse(1)
    # Documents that simdjson rejects but json.loads accepts are returned as
    # plain Python values, as loads would return them.
    assert fastsimdjson.parse("[1e400]") == [float("inf")]
    assert fastsimdjson.parse('"\\ud800"') == "\ud800"


def test_input_types():
    text = '{"k": ["v", 1]}'
    raw = text.encode()
    for arg in (text, raw, bytearray(raw), memoryview(raw)):
        assert fastsimdjson.parse(arg).as_dict() == {"k": ["v", 1]}


def test_big_array():
    # The element count saturates at 2**24 - 1 on the tape.
    n = (1 << 24) + 5
    a = fastsimdjson.parse(b"[" + b"0," * (n - 1) + b"7]")
    assert len(a) == n
    assert a[-1] == 7 and a[n - 2] == 0


def test_gc_state_is_preserved():
    d = fastsimdjson.parse(b'{"a": [1, 2, {"b": 3}]}')
    was = gc.isenabled()
    try:
        for enabled in (True, False):
            gc.enable() if enabled else gc.disable()
            d.as_dict()
            d["a"].as_list()
            assert gc.isenabled() == enabled
    finally:
        gc.enable() if was else gc.disable()


def test_release():
    d = fastsimdjson.parse(b'{"a": [1, 2]}')
    fastsimdjson.release()
    assert d.as_dict() == {"a": [1, 2]}
    del d
    fastsimdjson.release()
    assert fastsimdjson.parse(b"[3]").as_list() == [3]


def test_threads():
    docs = [json.dumps({"t": t, "v": list(range(t * 10))}) for t in range(8)]
    made = [fastsimdjson.parse(t) for t in docs]
    errors = []

    def work(t):
        try:
            # Read documents parsed by another thread, and parse new ones
            # whose views are dropped (and recycled) on this thread.
            for _ in range(200):
                assert to_py(made[(t + 1) % 8]) == json.loads(docs[(t + 1) % 8])
                assert fastsimdjson.parse(docs[t])["t"] == t
        except Exception as e:  # pragma: no cover
            errors.append(e)

    threads = [threading.Thread(target=work, args=(t,)) for t in range(8)]
    for th in threads:
        th.start()
    for th in threads:
        th.join()
    assert not errors
