"""Time dumps against json.dumps, orjson and msgspec on simdjson-data.

Two settings:
  default: json.dumps(obj) and fastsimdjson.dumps(obj) (identical str output);
  compact: separators=(",", ":"), ensure_ascii=False, the closest to what
           orjson.dumps and msgspec.json.encode produce (they return bytes).
"""
import glob
import json
import os
import sys
import time

import fastsimdjson

try:
    import orjson
except ImportError:
    orjson = None
try:
    import msgspec
except ImportError:
    msgspec = None

DATA = os.environ.get("JSONDIR", "simdjson-data/jsonexamples")
FILES = sys.argv[1:] or sorted(glob.glob(os.path.join(DATA, "*.json")))
COMPACT = {"separators": (",", ":"), "ensure_ascii": False}


def best(fn, obj, target=0.02, rounds=11):
    n = 1
    while True:
        t0 = time.perf_counter()
        for _ in range(n):
            fn(obj)
        if time.perf_counter() - t0 > target:
            break
        n *= 2
    b = float("inf")
    for _ in range(rounds):
        t0 = time.perf_counter()
        for _ in range(n):
            fn(obj)
        b = min(b, (time.perf_counter() - t0) / n)
    return b * 1e6


def main():
    cols = ["json", "fast", "json compact", "fast compact"]
    if orjson:
        cols.append("orjson")
    if msgspec:
        cols.append("msgspec")
    print(f"{'file':36s} " + " ".join(f"{c:>13s}" for c in cols) + "   (us)")
    for f in FILES:
        obj = json.loads(open(f, "rb").read())
        assert fastsimdjson.dumps(obj) == json.dumps(obj)
        assert fastsimdjson.dumps(obj, **COMPACT) == json.dumps(obj, **COMPACT)
        row = [best(json.dumps, obj), best(fastsimdjson.dumps, obj),
               best(lambda o: json.dumps(o, **COMPACT), obj),
               best(lambda o: fastsimdjson.dumps(o, **COMPACT), obj)]
        if orjson:
            row.append(best(orjson.dumps, obj))
        if msgspec:
            row.append(best(msgspec.json.encode, obj))
        print(f"{os.path.basename(f):36s} " + " ".join(f"{t:13.1f}" for t in row))


if __name__ == "__main__":
    main()
