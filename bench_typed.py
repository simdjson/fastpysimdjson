"""Time typed decoding: fastsimdjson.loads(data, type=T) against msgspec
(the same dataclasses, and msgspec Structs), pydantic (TypeAdapter.validate_json)
and orjson followed by cattrs. Reports the best of several runs in MB/s.

    python bench_typed.py [simdjson-data/jsonexamples] [gharchive.ndjson]

The cases:
  twitter  twitter.json, a few fields of each status and of its user
  citm     citm_catalog.json, typed in full
  gh       the first 64 MB of a GH Archive NDJSON file, six fields of each event
           (loads_many(type=...), one document per line for the others)
"""
import dataclasses
import os
import sys
import time
import typing
from typing import Any, Dict, List, Optional

import fastsimdjson

try:
    import msgspec
except ImportError:
    msgspec = None
try:
    import pydantic
except ImportError:
    pydantic = None
try:
    import cattrs
    import orjson
except ImportError:
    cattrs = None

ROUNDS = int(os.environ.get("ROUNDS", "7"))


# --- The types, as dataclasses -------------------------------------------------


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
    in_reply_to_status_id: Optional[int] = None


@dataclasses.dataclass
class Timeline:
    statuses: List[Status]


@dataclasses.dataclass
class Event:
    id: int
    name: Optional[str]
    description: Optional[str]
    logo: Optional[str]
    subtitle: Optional[str]
    subjectCode: Optional[str]
    topicIds: List[int]
    subTopicIds: List[int]


@dataclasses.dataclass
class Price:
    amount: int
    audienceSubCategoryId: int
    seatCategoryId: int


@dataclasses.dataclass
class Area:
    areaId: int
    blockIds: List[int]


@dataclasses.dataclass
class SeatCategory:
    areas: List[Area]
    seatCategoryId: int


@dataclasses.dataclass
class Performance:
    id: int
    eventId: int
    logo: Optional[str]
    name: Optional[str]
    prices: List[Price]
    seatCategories: List[SeatCategory]
    seatMapImage: Optional[str]
    start: int
    venueCode: str


@dataclasses.dataclass
class Catalog:
    areaNames: Dict[str, str]
    audienceSubCategoryNames: Dict[str, str]
    blockNames: Dict[str, str]
    events: Dict[str, Event]
    performances: List[Performance]
    seatCategoryNames: Dict[str, str]
    subTopicNames: Dict[str, str]
    subjectNames: Dict[str, str]
    topicNames: Dict[str, str]
    topicSubTopics: Dict[str, List[int]]
    venueNames: Dict[str, str]


@dataclasses.dataclass
class Actor:
    id: int
    login: str


@dataclasses.dataclass
class Repo:
    id: int
    name: str


@dataclasses.dataclass
class GhEvent:
    id: str
    type: str
    actor: Actor
    repo: Repo
    public: bool
    created_at: str


# --- The same types for msgspec (Struct) and pydantic --------------------------


def translate(t, make, memo):
    """t with each dataclass replaced by make(dataclass)."""
    if dataclasses.is_dataclass(t):
        if t not in memo:
            memo[t] = make(t, lambda h: translate(h, make, memo))
        return memo[t]
    origin, args = typing.get_origin(t), typing.get_args(t)
    if origin is typing.Union:
        return typing.Union[tuple(translate(a, make, memo) for a in args)]
    if origin is not None and args:
        return origin[tuple(translate(a, make, memo) for a in args)]
    return t


def field_specs(cls, tr):
    hints = typing.get_type_hints(cls)
    for f in dataclasses.fields(cls):
        has_default = f.default is not dataclasses.MISSING
        yield f.name, tr(hints[f.name]), has_default, f.default


def make_struct(cls, tr):
    fields = [(n, t, d) if has else (n, t) for n, t, has, d in field_specs(cls, tr)]
    return msgspec.defstruct(cls.__name__, fields)


def make_pydantic(cls, tr):
    fields = {n: (t, d if has else ...) for n, t, has, d in field_specs(cls, tr)}
    return pydantic.create_model(cls.__name__, **fields)


# --- Timing ----------------------------------------------------------------


def best(fn):
    t = float("inf")
    for _ in range(ROUNDS):
        t0 = time.perf_counter()
        fn()
        t = min(t, time.perf_counter() - t0)
    return t


def run(name, data, t, ndjson=False):
    lines = data.splitlines() if ndjson else None
    reference = (list(fastsimdjson.loads_many(data, type=t)) if ndjson
                 else fastsimdjson.loads(data, type=t))
    cands = {}
    if ndjson:
        cands["fastsimdjson.loads_many(type=)"] = lambda: list(fastsimdjson.loads_many(data, type=t))
        cands["fastsimdjson.loads_many (dicts)"] = lambda: list(fastsimdjson.loads_many(data))
    else:
        cands["fastsimdjson.loads(type=)"] = lambda: fastsimdjson.loads(data, type=t)
        cands["fastsimdjson.loads (dicts)"] = lambda: fastsimdjson.loads(data)
    if msgspec is not None:
        dec = msgspec.json.Decoder(t)
        sdec = msgspec.json.Decoder(translate(t, make_struct, {}))
        if ndjson:
            assert [dec.decode(x) for x in lines] == reference
            cands["msgspec (dataclass)"] = lambda: [dec.decode(x) for x in lines]
            cands["msgspec (Struct)"] = lambda: [sdec.decode(x) for x in lines]
        else:
            assert dec.decode(data) == reference
            cands["msgspec (dataclass)"] = lambda: dec.decode(data)
            cands["msgspec (Struct)"] = lambda: sdec.decode(data)
    if pydantic is not None:
        adapter = pydantic.TypeAdapter(translate(t, make_pydantic, {}))
        if ndjson:
            cands["pydantic"] = lambda: [adapter.validate_json(x) for x in lines]
        else:
            cands["pydantic"] = lambda: adapter.validate_json(data)
    if cattrs is not None:
        conv = cattrs.Converter()
        if ndjson:
            assert [conv.structure(orjson.loads(x), t) for x in lines] == reference
            cands["orjson + cattrs"] = lambda: [conv.structure(orjson.loads(x), t) for x in lines]
        else:
            assert conv.structure(orjson.loads(data), t) == reference
            cands["orjson + cattrs"] = lambda: conv.structure(orjson.loads(data), t)
    for who, fn in cands.items():
        s = best(fn)
        print(f"{name},{who},{s * 1e3:.2f},{len(data) / s / 1e6:.1f}", flush=True)


def main():
    jsondir = sys.argv[1] if len(sys.argv) > 1 else "simdjson-data/jsonexamples"
    print("case,library,ms,MBps")
    run("twitter", open(os.path.join(jsondir, "twitter.json"), "rb").read(), Timeline)
    run("citm", open(os.path.join(jsondir, "citm_catalog.json"), "rb").read(), Catalog)
    if len(sys.argv) > 2:
        with open(sys.argv[2], "rb") as f:
            data = f.read(64 << 20)
        run("gh", data[: data.rfind(b"\n") + 1], GhEvent, ndjson=True)


if __name__ == "__main__":
    main()
