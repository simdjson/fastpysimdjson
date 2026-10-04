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


# dumpb has the arguments, output and errors of orjson.dumps.

class Hue(enum.Enum):
    RED = "red"


class Level(enum.IntEnum):
    HIGH = 3


class MyStr2(str):
    pass


# Expected bytes recorded from orjson 3.12 (so that the tests do not need
# orjson); each case is (object, option, expected bytes or error message).
ORJSON_CASES = [
    ({"a": [1, 2.5, "x", True, None]}, 0, b'{"a":[1,2.5,"x",true,null]}'),
    ([0.0, -0.0, 1e16, 1e15, 1e-4, 1e-5, 2.5e-5, 1e-6, 1.5e-7, 1e-100, 5e-324, 1e22],
     0, b'[0.0,-0.0,1e+16,1000000000000000.0,0.0001,0.00001,0.000025,1e-6,1.5e-7,1e-100,5e-324,1e+22]'),
    ([float("nan"), float("inf"), float("-inf")], 0, b"[null,null,null]"),
    ([0, -1, 2**63 - 1, -(2**63), 2**64 - 1], 0,
     b"[0,-1,9223372036854775807,-9223372036854775808,18446744073709551615]"),
    (2**64, 0, "Integer exceeds 64-bit range"),
    (-(2**63) - 1, 0, "Integer exceeds 64-bit range"),
    (2**53, 64, "Integer exceeds 53-bit range"),
    (2**53 - 1, 64, b"9007199254740991"),
    ("\x00\x1f\"\\/\x7f\u00e9\u2028\U0001f600\b\f\n\r\t", 0,
     b'"\\u0000\\u001f\\"\\\\/\x7f\xc3\xa9\xe2\x80\xa8\xf0\x9f\x98\x80\\b\\f\\n\\r\\t"'),
    ("\ud800", 0, "str is not valid UTF-8: surrogates not allowed"),
    ({1: 2}, 0, "Dict key must be str"),
    ({True: 1, 2.5: 2, -3: 3, None: 4, Hue.RED: 5}, 4, b'{"true":1,"2.5":2,"-3":3,"null":4,"red":5}'),
    ({"b": 1, "a": [1, {}], "\u00e9": 2, "B": []}, 32 | 1 | 1024,
     b'{\n  "B": [],\n  "a": [\n    1,\n    {}\n  ],\n  "b": 1,\n  "\xc3\xa9": 2\n}\n'),
    ([{}, [], ()], 1, b"[\n  {},\n  [],\n  []\n]"),
    ([Hue.RED, Level.HIGH, MyStr2("s"), collections.OrderedDict(a=1), (1, 2)], 0,
     b'["red",3,"s",{"a":1},[1,2]]'),
    ({1, 2}, 0, "Type is not JSON serializable: set"),
    ([MyStr2("v")], 256, "Type is not JSON serializable: MyStr2"),
    ({MyStr2("k"): 1}, 256, "Dict key must be str"),
    (1, 1 << 20, "Invalid opts"),
]


def run_dumpb(obj, option=0, **kw):
    try:
        return fastsimdjson.dumpb(obj, option=option, **kw)
    except TypeError as e:
        return str(e)


@pytest.mark.parametrize("case", range(len(ORJSON_CASES)))
def test_dumpb_recorded(case):
    obj, option, expected = ORJSON_CASES[case]
    assert run_dumpb(obj, option) == expected


def test_dumpb_options_exported():
    assert fastsimdjson.OPT_INDENT_2 == 1 and fastsimdjson.OPT_SORT_KEYS == 32
    assert fastsimdjson.OPT_NON_STR_KEYS == 4 and fastsimdjson.OPT_APPEND_NEWLINE == 1024


def test_dumpb_default_and_depth():
    assert fastsimdjson.dumpb({1, 2}, default=sorted) == b"[1,2]"
    with pytest.raises(TypeError, match="default serializer exceeds recursion limit"):
        fastsimdjson.dumpb({1}, default=lambda o: {2})
    with pytest.raises(TypeError, match="Type is not JSON serializable: set") as info:
        fastsimdjson.dumpb({1}, default=lambda o: 1 / 0)
    assert isinstance(info.value.__cause__, ZeroDivisionError)
    for depth, ok in ((254, True), (255, False)):
        deep = []
        for _ in range(depth - 1):
            deep = [deep]
        if ok:
            assert fastsimdjson.dumpb(deep) == b"[" * depth + b"]" * depth
        else:
            with pytest.raises(TypeError, match="Recursion limit reached"):
                fastsimdjson.dumpb(deep)
    cycle = []
    cycle.append(cycle)
    with pytest.raises(TypeError, match="Recursion limit reached"):
        fastsimdjson.dumpb(cycle)


def test_dumpb_key_cache():
    # Keys are cached by identity: different keys at the same address over
    # time, and the same key in many dicts, must be written correctly.
    for i in range(3000):
        k = "key%d" % i
        assert fastsimdjson.dumpb({k: i}) == b'{"%s":%d}' % (k.encode(), i)
    key = "shared\u00e9\n"
    obj = [{key: i} for i in range(100)]
    assert fastsimdjson.dumpb(obj) == b"[" + b",".join(b'{"shared\xc3\xa9\\n":%d}' % i for i in range(100)) + b"]"
    fastsimdjson.release()
    assert fastsimdjson.dumpb({key: 1}) == b'{"shared\xc3\xa9\\n":1}'


orjson = None
try:
    import orjson
except ImportError:
    pass

ORJSON_OPTIONS = [0, 1, 32, 1 | 32, 4, 4 | 32, 1024, 64, 256]


@pytest.mark.skipif(orjson is None, reason="orjson not installed")
@pytest.mark.parametrize("path", sorted(glob.glob(os.path.join(DATA, "*.json"))))
def test_dumpb_matches_orjson_files(path):
    obj = json.loads(open(path, "rb").read())
    for option in ORJSON_OPTIONS:
        assert fastsimdjson.dumpb(obj, option=option) == orjson.dumps(obj, option=option)


def orjson_outcome(fn, obj, **kw):
    try:
        return ("ok", fn(obj, **kw))
    except TypeError as e:
        return ("TypeError", str(e))


@pytest.mark.skipif(orjson is None, reason="orjson not installed")
@pytest.mark.parametrize("seed", range(30))
def test_dumpb_matches_orjson_random(seed):
    obj = random_value(random.Random(seed))
    for option in ORJSON_OPTIONS:
        assert orjson_outcome(fastsimdjson.dumpb, obj, option=option) == \
            orjson_outcome(orjson.dumps, obj, option=option)


@pytest.mark.skipif(orjson is None, reason="orjson not installed")
def test_dumpb_matches_orjson_cases():
    for obj, option, _ in ORJSON_CASES:
        assert orjson_outcome(fastsimdjson.dumpb, obj, option=option) == \
            orjson_outcome(orjson.dumps, obj, option=option), (obj, option)
