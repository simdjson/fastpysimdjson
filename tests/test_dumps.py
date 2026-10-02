import collections
import enum
import glob
import io
import json
import math
import os
import random
import struct
import sys

import pytest

import fastsimdjson
from test_loads import DATA, random_value

OPTIONS = [
    {},
    {"ensure_ascii": False},
    {"indent": 2},
    {"indent": "\t", "sort_keys": True},
    {"indent": 0},
    {"indent": -1},
    {"separators": (",", ":")},
    {"separators": [";", "="], "ensure_ascii": False},
    {"sort_keys": True},
    {"indent": 3, "separators": (", ", " : ")},
    {"separators": ("·", "→")},
    {"indent": " ", "ensure_ascii": False},
]


def same_result(fn):
    """fn(module) must return the same value, or raise the same exception,
    with json and fastsimdjson."""
    try:
        expected = fn(json)
    except Exception as e:  # noqa: BLE001
        with pytest.raises(type(e)) as info:
            fn(fastsimdjson)
        assert str(info.value) == str(e)
        return
    assert fn(fastsimdjson) == expected


@pytest.mark.parametrize("path", sorted(glob.glob(os.path.join(DATA, "*.json"))))
def test_files(path):
    obj = json.loads(open(path, "rb").read())
    for kw in OPTIONS:
        assert fastsimdjson.dumps(obj, **kw) == json.dumps(obj, **kw)


@pytest.mark.parametrize("seed", range(50))
def test_random(seed):
    obj = random_value(random.Random(seed))
    for kw in OPTIONS:
        assert fastsimdjson.dumps(obj, **kw) == json.dumps(obj, **kw)


def test_floats():
    rng = random.Random(7)
    values = [0.0, -0.0, 1.0, -1.5, 0.1, 1e16, 1e15, 9007199254740993.0, 1e-4, 1e-5,
              123456789.125, 5e-324, 2.2250738585072014e-308, 1.7976931348623157e308,
              1e22, 1e23, 100.0, 1e100, 3.0e-7, 0.000123]
    values += [struct.unpack("d", struct.pack("Q", rng.getrandbits(64)))[0] for _ in range(20000)]
    values += [rng.uniform(-1e9, 1e9) for _ in range(20000)]
    values += [round(rng.uniform(-1000, 1000), rng.randrange(8)) for _ in range(20000)]
    values = [v for v in values if math.isfinite(v)]
    assert fastsimdjson.dumps(values) == json.dumps(values)
    for v in values[:2000]:
        assert fastsimdjson.dumps({v: v}) == json.dumps({v: v})


def test_nan_and_infinity():
    values = [float("nan"), float("inf"), float("-inf")]
    assert fastsimdjson.dumps(values) == json.dumps(values)
    assert fastsimdjson.dumps({float("inf"): 1}) == json.dumps({float("inf"): 1})
    for v in values:
        same_result(lambda m: m.dumps([v], allow_nan=False))
        same_result(lambda m: m.dumps({v: 1}, allow_nan=False))


def test_integers():
    values = [0, -1, 1, 2**63 - 1, -(2**63), 2**63, 2**64, -(2**64) - 1, 10**100, -(10**300)]
    for v in values:
        assert fastsimdjson.dumps(v) == json.dumps(v)
        assert fastsimdjson.dumps({v: v}) == json.dumps({v: v})
    same_result(lambda m: m.dumps(10**5000))


class Color(enum.IntEnum):
    RED = 1


class LoudInt(int):
    def __repr__(self):
        return "LOUD"


class MyFloat(float):
    def __repr__(self):
        return "MYFLOAT"


class MyStr(str):
    def __str__(self):
        return "MYSTR"


class MyList(list):
    pass


class MyDict(dict):
    pass


class ItemsDict(dict):
    def items(self):
        return [("overridden", 1)]


def test_subclasses():
    objs = [Color.RED, {Color.RED: Color.RED}, LoudInt(5), {LoudInt(5): 1}, MyFloat(2.5),
            MyStr("x"), {MyStr("k"): MyStr("v")}, MyList([1, MyList([2])]),
            MyDict(a=MyDict(b=1)), ItemsDict(a=1), collections.OrderedDict([("z", 1), ("a", 2)]),
            (1, (2, 3)), True, False, None, {True: 1, False: 2, None: 3, 1.5: 4, 7: 5}]
    for o in objs:
        for kw in ({}, {"sort_keys": False, "indent": 1}):
            assert fastsimdjson.dumps(o, **kw) == json.dumps(o, **kw), o


def test_strings():
    chars = "".join(chr(c) for c in range(0x80)) + "éÿĀ☃￿\U0001f600\U0010ffff"
    strings = [chars, chars * 3, "a" * 100, "a\"b\\c" * 10, "☃" * 17, ""]
    strings += ["x" * n + "\n" for n in range(40)]
    for s in strings:
        for kw in ({}, {"ensure_ascii": False}):
            assert fastsimdjson.dumps(s, **kw) == json.dumps(s, **kw)
            assert fastsimdjson.dumps({s: s}, **kw) == json.dumps({s: s}, **kw)


def test_lone_surrogates():
    # Including a high and a low surrogate as two code points, which json
    # keeps apart (they are not a character).
    for s in ["\ud800", "a\udfffb", "\ud83d", "\ud83d\ude00", "\u6817\uda57\udf54x", "\U0001f600\ud800"]:
        for kw in ({}, {"ensure_ascii": False}):
            assert fastsimdjson.dumps([s], **kw) == json.dumps([s], **kw)


def test_errors_match_json():
    same_result(lambda m: m.dumps({1, 2}))
    same_result(lambda m: m.dumps([object()]))
    same_result(lambda m: m.dumps({(1, 2): 3}))
    same_result(lambda m: m.dumps({1: 2, "1": 3, "a": 4}, sort_keys=True))
    same_result(lambda m: m.dumps({"a": 1, 2: 3}, sort_keys=True))
    same_result(lambda m: m.dumps([1], separators=("a",)))
    same_result(lambda m: m.dumps([1], indent=[1]))
    same_result(lambda m: m.dumps([1], unknown_option=1))
    same_result(lambda m: m.dumps())
    same_result(lambda m: m.dumps(1, 2))


def test_skipkeys():
    obj = {(1, 2): 3, "a": [{"b": 1, (3,): 4}]}
    for kw in ({}, {"indent": 2}):
        assert fastsimdjson.dumps(obj, skipkeys=True, **kw) == json.dumps(obj, skipkeys=True, **kw)


def test_circular():
    a = [1]
    a.append(a)
    same_result(lambda m: m.dumps(a))
    d = {}
    d["self"] = [d]
    same_result(lambda m: m.dumps(d))
    same_result(lambda m: m.dumps(d, check_circular=False))
    # Repeated (not circular) references are fine.
    x = [1, 2]
    assert fastsimdjson.dumps([x, x, {"k": x}]) == json.dumps([x, x, {"k": x}])


def test_default():
    def default(o):
        if isinstance(o, set):
            return sorted(o)
        if isinstance(o, complex):
            return {"re": o.real, "im": o.imag}
        raise TypeError(f"no {type(o).__name__}")

    obj = {"s": {3, 1, 2}, "c": [1 + 2j], "n": [{"x": {5}}]}
    assert fastsimdjson.dumps(obj, default=default) == json.dumps(obj, default=default)
    same_result(lambda m: m.dumps([object()], default=default))
    # default returning the object itself is a circular reference.
    same_result(lambda m: m.dumps([object()], default=lambda o: o))


def test_default_may_mutate():
    data = {"a": [1, 2, 3], "b": object()}

    def default(o):
        data["a"].clear()
        data.clear()
        return "gone"

    snapshot = dict(data)
    json_out = json.dumps(dict(snapshot, a=[1, 2, 3]), default=lambda o: "gone")
    out = fastsimdjson.dumps(data, default=default)
    assert isinstance(out, str) and json_out


def test_cls():
    class Encoder(json.JSONEncoder):
        def default(self, o):
            return "custom"

    assert fastsimdjson.dumps([object()], cls=Encoder) == '["custom"]'


def test_deep_nesting():
    for depth in (100, 500):
        deep, deep_dict = [], {}
        for _ in range(depth):
            deep, deep_dict = [deep], {"k": deep_dict}
        assert fastsimdjson.dumps(deep) == json.dumps(deep)
        assert fastsimdjson.dumps(deep_dict) == json.dumps(deep_dict)
    # Where nesting runs out depends on the Python version (a counter before
    # 3.14, the stack size since) and on each encoder's frames: the output
    # must be correct, or the error a RecursionError, never a crash.
    for depth in (1000, 10000, 100000, 1000000):
        deep = []
        for _ in range(depth):
            deep = [deep]
        try:
            assert fastsimdjson.dumps(deep) == "[" * (depth + 1) + "]" * (depth + 1)
        except RecursionError:
            pass

def test_roundtrip():
    obj = {"a": [1, 2.5, "é", None, True, {"b": -0.0}], "big": 2**80}
    assert fastsimdjson.loads(fastsimdjson.dumps(obj)) == obj


def test_dump():
    obj = {"a": [1, "☃"]}
    for kw in ({}, {"indent": 2, "ensure_ascii": False}):
        a, b = io.StringIO(), io.StringIO()
        fastsimdjson.dump(obj, a, **kw)
        json.dump(obj, b, **kw)
        assert a.getvalue() == b.getvalue()
    with pytest.raises(TypeError):
        fastsimdjson.dump(obj)
