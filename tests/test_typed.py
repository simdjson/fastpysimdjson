"""loads(data, type=T): typed decoding into dataclasses, TypedDicts,
NamedTuples, enums, containers, unions, literals and scalars."""
import dataclasses
import enum
import json
import math
import os
import random
import sys
import typing
from typing import Any, Dict, List, Literal, NamedTuple, Optional, Tuple, TypedDict, Union

import pytest

import fastsimdjson
from fastsimdjson import ValidationError, loads

try:
    import msgspec
except ImportError:
    msgspec = None

DATA = os.environ.get("JSONDIR", "simdjson-data/jsonexamples")


class Color(enum.Enum):
    RED = "red"
    GREEN = "green"


class Level(enum.IntEnum):
    LOW = 1
    HIGH = 2


@dataclasses.dataclass
class User:
    id: int
    name: str
    tags: List[str] = dataclasses.field(default_factory=list)
    score: float = 0.0
    color: Optional[Color] = None


@dataclasses.dataclass(frozen=True)
class Point:
    x: float
    y: float


@dataclasses.dataclass(slots=True)
class Slotted:
    a: int
    b: str = "b"


@dataclasses.dataclass
class Node:
    value: int
    children: "List[Node]" = dataclasses.field(default_factory=list)
    parent: "Optional[Node]" = None


@dataclasses.dataclass
class WithPostInit:
    a: int
    double: int = dataclasses.field(init=False, default=0)

    def __post_init__(self):
        if self.a < 0:
            raise ValueError("a must be positive")
        self.double = 2 * self.a


class Pair(NamedTuple):
    a: int
    b: str = "z"


class Movie(TypedDict):
    title: str
    year: int


class Partial(TypedDict, total=False):
    title: str
    year: int


def test_scalars():
    assert loads(b"1", type=int) == 1
    assert loads(b"-5", type=int) == -5
    assert loads(b"18446744073709551615", type=int) == 18446744073709551615
    assert loads(b"123456789012345678901234567890", type=int) == 123456789012345678901234567890
    assert loads(b"1.5", type=float) == 1.5
    x = loads(b"2", type=float)
    assert x == 2.0 and type(x) is float
    assert type(loads(b"123456789012345678901234567890", type=float)) is float
    assert loads(b'"\\u00e9t\\u00e9"', type=str) == "été"
    assert loads(b"true", type=bool) is True
    assert loads(b"null", type=None) is None
    assert loads(b"null", type=type(None)) is None
    assert loads(b'{"a": [1, null]}', type=Any) == {"a": [1, None]}
    assert loads(b'{"a": [1, null]}', type=object) == {"a": [1, None]}


@pytest.mark.parametrize("data, t, message", [
    (b'"1"', int, "Expected `int`, got `str`"),
    (b"1.0", int, "Expected `int`, got `float`"),
    (b"true", int, "Expected `int`, got `bool`"),
    (b"1", bool, "Expected `bool`, got `int`"),
    (b"null", str, "Expected `str`, got `null`"),
    (b'"x"', float, "Expected `float`, got `str`"),
    (b"[]", dict, "Expected `object`, got `array`"),
    (b"{}", list, "Expected `array`, got `object`"),
    (b"1", None, None),  # type=None: no typing
    (b'"x"', Optional[int], "Expected `null | int`, got `str`"),
])
def test_scalar_errors(data, t, message):
    if message is None:
        assert loads(data, type=t) == json.loads(data)
        return
    with pytest.raises(ValidationError) as e:
        loads(data, type=t)
    assert str(e.value) == message
    assert isinstance(e.value, ValueError)


def test_containers():
    assert loads(b"[1, 2]", type=List[int]) == [1, 2]
    assert loads(b"[1, 2]", type=list) == [1, 2]
    assert loads(b"[1, 2]", type=typing.Sequence[int]) == [1, 2]
    assert loads(b"[1, 2, 2]", type=set[int]) == {1, 2}
    assert loads(b"[1, 2, 2]", type=frozenset[int]) == frozenset({1, 2})
    assert loads(b"[1, 2]", type=Tuple[int, ...]) == (1, 2)
    assert loads(b"[1, 2]", type=tuple) == (1, 2)
    assert loads(b'[1, "a"]', type=Tuple[int, str]) == (1, "a")
    assert loads(b"[]", type=Tuple[()]) == ()
    assert loads(b'{"a": 1}', type=Dict[str, int]) == {"a": 1}
    assert loads(b'{"a": 1}', type=typing.Mapping[str, float]) == {"a": 1.0}
    assert loads(b'{"a": {"b": [1.5]}}', type=dict[str, dict[str, list[float]]]) == {
        "a": {"b": [1.5]}}
    with pytest.raises(ValidationError, match=r"Expected `array` of length 2, got 3"):
        loads(b"[1, 2, 3]", type=Tuple[int, int])
    with pytest.raises(ValidationError, match=r"at `\$\[1\]`"):
        loads(b'[1, "a"]', type=List[int])
    with pytest.raises(ValidationError, match=r"at `\$.b`"):
        loads(b'{"a": 1, "b": "x"}', type=Dict[str, int])
    with pytest.raises(TypeError, match="str keys"):
        loads(b"{}", type=Dict[int, int])


def test_unions_and_literals():
    t = List[Union[int, str, None]]
    assert loads(b'[1, "a", null]', type=t) == [1, "a", None]
    assert loads(b"[1, 2.5]", type=List[Union[int, float]]) == [1, 2.5]
    assert type(loads(b"[1]", type=List[Union[float, int]])[0]) is int
    assert loads(b'{"a": [1]}', type=Union[List[int], Dict[str, List[int]]]) == {"a": [1]}
    assert loads(b"1", type=int | None) == 1
    assert loads(b'"a"', type=Union[int, Any]) == "a"
    with pytest.raises(ValidationError, match=r"Expected `int \| str`, got `float`"):
        loads(b"1.5", type=Union[int, str])
    with pytest.raises(TypeError, match="more than one"):
        loads(b"{}", type=Union[User, Point])
    assert loads(b'"a"', type=Literal["a", "b"]) == "a"
    assert loads(b"2", type=Literal[1, 2, None]) == 2
    assert loads(b"null", type=Literal[1, 2, None]) is None
    with pytest.raises(ValidationError, match="Invalid value 'c'"):
        loads(b'"c"', type=Literal["a", "b"])
    with pytest.raises(ValidationError, match="Expected `str`, got `int`"):
        loads(b"1", type=Literal["a", "b"])


def test_union_of_every_kind_still_checks():
    # Every JSON kind is accepted, but the parts of the union are still checked.
    t = Union[Dict[str, int], List[int], str, int, float, bool, None]
    assert loads(b'{"a": 1}', type=t) == {"a": 1}
    with pytest.raises(ValidationError, match=r"at `\$\[0\]`"):
        loads(b'["z"]', type=t)
    with pytest.raises(ValidationError, match=r"at `\$.a`"):
        loads(b'{"a": "z"}', type=t)
    u = loads(b'{"id": 1, "name": "a"}', type=Union[User, int, str, float, bool, None, list])
    assert u == User(1, "a")


def test_error_names():
    with pytest.raises(ValidationError, match=r"^Expected `int \| float`, got `str`$"):
        loads(b'"s"', type=Union[int, float])
    with pytest.raises(ValidationError, match=r"^Expected `null \| float`, got `str`$"):
        loads(b'"s"', type=Optional[float])
    with pytest.raises(ValidationError, match=r"^Expected `float`, got `str`$"):
        loads(b'"s"', type=float)


def test_lone_surrogate_keys():
    # Only json accepts this document (NaN), and its first key is a lone
    # surrogate, which UTF-8 cannot encode.
    data = b'{"\\ud800": 1, "id": 3, "name": "a", "x": NaN}'
    assert list(loads(data, type=dict)) == ["\ud800", "id", "name", "x"]
    assert loads(data, type=Dict[str, Any])["\ud800"] == 1
    assert loads(data, type=User) == User(3, "a")
    with pytest.raises(ValidationError, match=r"at `\$.\?`"):
        loads(b'{"\\ud800": "s", "x": NaN}', type=Dict[str, int])


def test_enums():
    assert loads(b'"red"', type=Color) is Color.RED
    assert loads(b"2", type=Level) is Level.HIGH
    assert loads(b'["green", "red"]', type=List[Color]) == [Color.GREEN, Color.RED]
    with pytest.raises(ValidationError, match="Invalid value 'blue'"):
        loads(b'"blue"', type=Color)
    with pytest.raises(ValidationError, match="Expected `str`, got `int`"):
        loads(b"1", type=Color)


def test_dataclasses():
    u = loads(b'{"id": 1, "name": "a", "tags": ["x"], "score": 3, "color": "red"}', type=User)
    assert u == User(1, "a", ["x"], 3.0, Color.RED)
    assert type(u.score) is float
    # Defaults, a fresh list from the factory each time, unknown keys ignored.
    a = loads(b'{"name": "a", "id": 1, "other": {"deep": [1, 2, {"x": null}]}}', type=User)
    b = loads(b'{"id": 2, "name": "b"}', type=User)
    assert a == User(1, "a") and b == User(2, "b")
    assert a.tags is not b.tags
    # A key that appears twice: the last one wins, as with dicts.
    assert loads(b'{"id": 1, "name": "a", "id": 2}', type=User).id == 2
    with pytest.raises(ValidationError, match="Object missing required field `name`"):
        loads(b'{"id": 1}', type=User)
    with pytest.raises(ValidationError, match=r"Expected `int`, got `str` - at `\$.id`"):
        loads(b'{"id": "1", "name": "a"}', type=User)
    with pytest.raises(ValidationError, match=r"at `\$.tags\[1\]`"):
        loads(b'{"id": 1, "name": "a", "tags": ["a", 2]}', type=User)


def test_frozen_slots_post_init():
    p = loads(b'{"x": 1, "y": 2.5}', type=Point)
    assert p == Point(1.0, 2.5)
    with pytest.raises(dataclasses.FrozenInstanceError):
        p.x = 3
    s = loads(b'{"a": 1}', type=Slotted)
    assert s == Slotted(1) and not hasattr(s, "__dict__")
    w = loads(b'{"a": 3, "double": 100}', type=WithPostInit)
    assert w.a == 3 and w.double == 6
    with pytest.raises(ValidationError, match=r"a must be positive - at `\$\[0\]`") as e:
        loads(b'[{"a": -1}]', type=List[WithPostInit])
    assert isinstance(e.value.__cause__, ValueError)


def test_recursive():
    data = b'{"value": 1, "children": [{"value": 2}, {"value": 3, "children": [{"value": 4}]}]}'
    n = loads(data, type=Node)
    assert n == Node(1, [Node(2), Node(3, [Node(4)])])
    assert loads(b'{"value": 1, "parent": {"value": 0}}', type=Node).parent == Node(0)
    with pytest.raises(ValidationError, match=r"at `\$.children\[0\].children\[0\].value`"):
        loads(b'{"value": 1, "children": [{"value": 2, "children": [{"value": "x"}]}]}', type=Node)


def test_namedtuple_and_typeddict():
    assert loads(b'[1, "a"]', type=Pair) == Pair(1, "a")
    assert loads(b"[1]", type=Pair) == Pair(1, "z")
    with pytest.raises(ValidationError, match="length 1 to 2, got 3"):
        loads(b"[1, 2, 3]", type=Pair)
    with pytest.raises(ValidationError, match=r"at `\$\[1\]`"):
        loads(b"[1, 2]", type=Pair)
    m = loads(b'{"year": 1999, "title": "Matrix", "rating": 9}', type=Movie)
    assert m == {"title": "Matrix", "year": 1999} and type(m) is dict
    with pytest.raises(ValidationError, match="missing required field `year`"):
        loads(b'{"title": "Matrix"}', type=Movie)
    assert loads(b'{"year": 1999}', type=Partial) == {"year": 1999}


def test_input_types_and_fallback():
    for data in (b'{"x": 1, "y": 2}', '{"x": 1, "y": 2}', bytearray(b'{"x": 1, "y": 2}'),
                 memoryview(b'{"x": 1, "y": 2}')):
        assert loads(data, type=Point) == Point(1, 2)
    # Documents that only json accepts are converted the same way.
    p = loads(b'{"x": NaN, "y": 1e400}', type=Point)
    assert math.isnan(p.x) and p.y == math.inf
    with pytest.raises(ValidationError, match=r"at `\$.y`"):
        loads(b'{"x": NaN, "y": "a"}', type=Point)
    with pytest.raises(fastsimdjson.JSONDecodeError):
        loads(b'{"x": 1,', type=Point)
    with pytest.raises(TypeError, match="not supported"):
        loads(b"1", type=bytes)
    with pytest.raises(TypeError):
        loads(b"1", type=int, other=1)
    with pytest.raises(TypeError):
        loads(type=int)


def test_files(tmp_path):
    path = tmp_path / "p.json"
    path.write_bytes(b'{"x": 1, "y": 2}')
    assert fastsimdjson.load_file(str(path), type=Point) == Point(1, 2)
    assert fastsimdjson.load_file(str(path)) == {"x": 1, "y": 2}
    with open(path, "rb") as f:
        assert fastsimdjson.load(f, type=Point) == Point(1, 2)


def test_streams():
    data = b'{"x": 1, "y": 2}\n{"x": 3, "y": 4}\n'
    assert list(fastsimdjson.loads_many(data, type=Point)) == [Point(1, 2), Point(3, 4)]
    assert fastsimdjson.map_many(lambda p: p.x + p.y, data, type=Point, threads=2) == [3.0, 7.0]
    big = b"".join(b'{"x": %d, "y": %d}\n' % (i, -i) for i in range(50000))
    assert fastsimdjson.loads_many(big, type=Point, threads=4) == [
        Point(i, -i) for i in range(50000)]
    bad = big + b'{"x": 1, "y": "a"}\n'
    with pytest.raises(ValidationError, match=r"at `\$.y`"):
        fastsimdjson.loads_many(bad, type=Point, threads=4)
    with pytest.raises(ValidationError):
        list(fastsimdjson.loads_many(bad, type=Point))
    # NaN goes through json, then the conversion.
    out = list(fastsimdjson.loads_many(b'{"x": NaN, "y": 1}\n{"x": 2, "y": 3}\n', type=Point))
    assert math.isnan(out[0].x) and out[1] == Point(2, 3)
    with pytest.raises(ValueError, match="fields"):
        fastsimdjson.loads_many(data, type=Point, fields="/x")
    with pytest.raises(ValueError, match="lazy"):
        fastsimdjson.map_many(lambda p: p, data, type=Point, lazy=True)


def test_plan_is_cached():
    for _ in range(3):
        assert loads(b"[1]", type=list[int]) == [1]
    # An unhashable type annotation still works (it is not cached).
    t = typing.Annotated[int, {"unhashable": []}]
    assert loads(b"1", type=t) == 1


def test_threads():
    # Threads that describe and use the same types at once (on free-threaded
    # Python, really at once).
    import concurrent.futures

    def work(i):
        @dataclasses.dataclass
        class Local:
            a: int
            b: List[Point]
        for _ in range(200):
            assert loads(b'{"a": 1, "b": [{"x": 1, "y": 2}]}', type=Local) == Local(1, [Point(1, 2)])
            assert loads(b'[%d]' % i, type=List[int]) == [i]
            assert loads(b'{"id": 1, "name": "a"}', type=User) == User(1, "a")
        return True

    with concurrent.futures.ThreadPoolExecutor(8) as ex:
        assert all(ex.map(work, range(16)))


# --- Differential tests against msgspec ------------------------------------


@dataclasses.dataclass
class Item:
    sku: str
    qty: int
    price: float


@dataclasses.dataclass
class Order:
    id: int
    customer: Dict[str, str]
    items: List[Item]
    status: Literal["new", "paid", "shipped"]
    color: Color
    note: Optional[str] = None
    tags: Tuple[str, ...] = ()
    extra: Any = None


def random_order(rng):
    def text():
        return "".join(rng.choice("abé\"\\\n中\U0001f600") for _ in range(rng.randrange(6)))
    o = {
        "id": rng.randrange(-2**63, 2**63),
        "customer": {text(): text() for _ in range(rng.randrange(3))},
        "items": [{"sku": text(), "qty": rng.randrange(100), "price": rng.uniform(-1e6, 1e6)}
                  for _ in range(rng.randrange(4))],
        "status": rng.choice(["new", "paid", "shipped"]),
        "color": rng.choice(["red", "green"]),
    }
    if rng.random() < 0.5:
        o["note"] = rng.choice([None, text()])
    if rng.random() < 0.5:
        o["tags"] = [text() for _ in range(rng.randrange(3))]
    if rng.random() < 0.5:
        o["extra"] = rng.choice([None, 1, [1, {"a": "b"}], {"x": [True, 2.5]}])
    if rng.random() < 0.3:
        o["unknown"] = {"deep": [o["items"]]}
    keys = list(o)
    rng.shuffle(keys)
    return {k: o[k] for k in keys}


def corrupt(rng, doc):
    """A copy of doc with one value replaced by a value of another type."""
    doc = json.loads(json.dumps(doc))
    target = doc
    while True:
        if isinstance(target, dict) and target:
            k = rng.choice(list(target))
        elif isinstance(target, list) and target:
            k = rng.randrange(len(target))
        else:
            return None
        if rng.random() < 0.4 or not isinstance(target[k], (dict, list)) or not target[k]:
            target[k] = rng.choice([1, 1.5, "s", None, True, [], {}])
            return doc
        target = target[k]


@pytest.mark.skipif(msgspec is None, reason="msgspec is not installed")
@pytest.mark.parametrize("seed", range(20))
def test_against_msgspec(seed):
    rng = random.Random(seed)
    for _ in range(50):
        doc = random_order(rng)
        data = json.dumps(doc).encode()
        assert loads(data, type=Order) == msgspec.json.decode(data, type=Order)
        bad = corrupt(rng, doc)
        if bad is None:
            continue
        data = json.dumps(bad).encode()
        try:
            expected = msgspec.json.decode(data, type=Order)
        except msgspec.ValidationError:
            with pytest.raises(ValidationError):
                loads(data, type=Order)
        else:
            assert loads(data, type=Order) == expected


@pytest.mark.skipif(msgspec is None, reason="msgspec is not installed")
def test_twitter_against_msgspec():
    path = os.path.join(DATA, "twitter.json")
    if not os.path.exists(path):
        pytest.skip("simdjson-data is not there")

    @dataclasses.dataclass
    class TwitterUser:
        id: int
        screen_name: str
        followers_count: int
        description: Optional[str] = None

    @dataclasses.dataclass
    class Status:
        id: int
        text: str
        user: TwitterUser
        retweet_count: int
        favorited: bool
        entities: Dict[str, List[Any]]
        in_reply_to_status_id: Optional[int] = None

    @dataclasses.dataclass
    class Timeline:
        statuses: List[Status]

    data = open(path, "rb").read()
    assert loads(data, type=Timeline) == msgspec.json.decode(data, type=Timeline)


def test_typing_extensions_typeddict():
    te = pytest.importorskip("typing_extensions")

    class Movie(te.TypedDict):
        title: str
        year: te.NotRequired[int]

    class Partial(te.TypedDict, total=False):
        title: te.Required[str]
        rating: float

    assert loads(b'{"title": "x", "year": 1999}', type=Movie) == {"title": "x", "year": 1999}
    assert loads(b'{"title": "x"}', type=Movie) == {"title": "x"}
    assert loads(b'{"title": "x", "rating": 2}', type=Partial) == {"title": "x", "rating": 2.0}
    with pytest.raises(ValidationError, match="missing required field `title`"):
        loads(b'{"rating": 2}', type=Partial)
    with pytest.raises(ValidationError, match=r"at `\$\.year`"):
        loads(b'{"title": "x", "year": "1999"}', type=Movie)


def test_empty_tuple_type():
    for t in (tuple[()], Tuple[()]):
        assert loads(b"[]", type=t) == ()
        with pytest.raises(ValidationError, match="length 0, got 2"):
            loads(b"[1, 2]", type=t)
    for t in (tuple, Tuple):
        assert loads(b"[1, 2]", type=t) == (1, 2)


def test_literal_bools_are_not_ints():
    assert loads(b"1", type=Literal[1, False]) == 1
    assert loads(b"false", type=Literal[1, False]) is False
    for data, t in [(b"0", Literal[1, False]), (b"true", Literal[1, False]),
                    (b"1", Literal[True, 0]), (b"false", Literal[True, 0])]:
        with pytest.raises(ValidationError, match="Invalid value"):
            loads(data, type=t)


def test_set_elements_must_be_hashable():
    @dataclasses.dataclass
    class Point:
        x: int

    @dataclasses.dataclass(frozen=True)
    class FrozenPoint:
        x: int

    for t in (set[list[int]], frozenset[dict[str, int]], set[Point], typing.Set[set[int]],
              set[Optional[List[int]]], frozenset[tuple[int, list[int]]]):
        with pytest.raises(TypeError, match="Set elements must be hashable"):
            loads(b"[]", type=t)
    assert loads(b'[{"x": 1}, {"x": 1}]', type=frozenset[FrozenPoint]) == {FrozenPoint(1)}
    assert loads(b"[[1, 2], [1, 2]]", type=set[tuple[int, ...]]) == {(1, 2)}


def test_float_out_of_range():
    with pytest.raises(ValidationError, match=r"out of range for `float` - at `\$\[1\]`"):
        loads(b"[1, 1" + b"0" * 400 + b"]", type=list[float])
    assert loads(b"[1" + b"0" * 30 + b"]", type=list[float]) == [1e30]


@dataclasses.dataclass
class Tree:
    kids: List["Tree"]


def test_deep_nesting_does_not_crash():
    # Past simdjson's depth limit, json decodes the document; then the depth
    # is checked as json checks it: a RecursionError, never a crash.
    limit = sys.getrecursionlimit()
    sys.setrecursionlimit(max(limit, 5000))  # Python 3.10 and 3.11 count C levels there
    try:
        n = 1000  # 2000 levels
        tree = loads(b'{"kids":[' * n + b']}' * n, type=Tree)
        for _ in range(n - 1):
            tree = tree.kids[0]
        assert tree.kids == []
        for n in (20000, 200000):
            data = b'{"kids":[' * n + b']}' * n
            try:
                loads(data, type=Tree)
            except RecursionError:
                pass
    finally:
        sys.setrecursionlimit(limit)


def test_union_that_contains_itself():
    ns = {}
    try:
        exec("type Loop = Loop | None\ntype Self = Self", ns)
    except SyntaxError:
        pytest.skip("type statement needs Python 3.12")
    with pytest.raises(TypeError, match="contains itself"):
        loads(b"1", type=ns["Loop"])
    with pytest.raises(TypeError, match="refers only to itself"):
        loads(b"1", type=ns["Self"])


def test_reentrant_loads():
    # The user's code may call loads while a typed document is decoded: it
    # must not reuse the parser, whose document is being read.
    inner = b'{"xs": [' + b",".join(b"%d" % i for i in range(3000)) + b']}'

    @dataclasses.dataclass
    class Inner:
        xs: List[int]

    class Mood(enum.Enum):
        HAPPY = "happy"

        @classmethod
        def _missing_(cls, value):
            loads(inner)
            fastsimdjson.release()
            return cls.HAPPY

    @dataclasses.dataclass
    class A:
        a: int

        def __post_init__(self):
            assert len(loads(inner, type=Inner).xs) == 3000
            fastsimdjson.release()

    @dataclasses.dataclass
    class Outer:
        first: A
        mood: Mood
        rest: str
        tail: List[str]

    doc = b'{"first": {"a": 1}, "mood": "sad", "rest": "a string that follows", "tail": ["x", "y"]}'
    for _ in range(3):
        assert loads(doc, type=Outer) == Outer(A(1), Mood.HAPPY, "a string that follows", ["x", "y"])
    assert [o.rest for o in fastsimdjson.loads_many(doc + b"\n" + doc, type=Outer)] == \
        ["a string that follows"] * 2


def test_reentrant_stream_next():
    stream = None

    @dataclasses.dataclass
    class Item:
        i: int

        def __post_init__(self):
            next(stream)

    stream = fastsimdjson.loads_many(b'{"i": 1}\n{"i": 2}\n', type=Item)
    with pytest.raises(RuntimeError, match="already executing"):
        next(stream)


def test_type_cache_is_bounded():
    for i in range(3000):
        cls = dataclasses.make_dataclass(f"C{i}", [("x", int)])
        assert loads(b'{"x": 1}', type=cls) == cls(1)
    import weakref
    ref = weakref.ref(cls)
    for i in range(1100):
        loads(b'{"x": 1}', type=dataclasses.make_dataclass(f"D{i}", [("x", int)]))
    del cls
    import gc
    gc.collect()
    assert ref() is None
