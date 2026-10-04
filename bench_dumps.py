"""Time dumps and dumpb against json.dumps, orjson and msgspec on simdjson-data.

Two comparisons:
  str:   json.dumps(obj) against fastsimdjson.dumps(obj), the same str;
  bytes: UTF-8 bytes in the compact form that orjson and msgspec produce
         (no spaces, non-ASCII characters not escaped):
         json.dumps(obj, separators=(",", ":"), ensure_ascii=False).encode(),
         fastsimdjson.dumpb(obj, separators=(",", ":"), ensure_ascii=False),
         orjson.dumps(obj) and msgspec.json.encode(obj).
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
    cols = ["json str", "fast str", "json bytes", "fast bytes"]
    if orjson:
        cols.append("orjson")
    if msgspec:
        cols.append("msgspec")
    print(f"{'file':36s} " + " ".join(f"{c:>11s}" for c in cols) + "   (us)")
    for f in FILES:
        obj = json.loads(open(f, "rb").read())
        assert fastsimdjson.dumps(obj) == json.dumps(obj)
        assert fastsimdjson.dumpb(obj, **COMPACT) == json.dumps(obj, **COMPACT).encode()
        row = [best(json.dumps, obj), best(fastsimdjson.dumps, obj),
               best(lambda o: json.dumps(o, **COMPACT).encode(), obj),
               best(lambda o: fastsimdjson.dumpb(o, **COMPACT), obj)]
        if orjson:
            row.append(best(orjson.dumps, obj))
        if msgspec:
            row.append(best(msgspec.json.encode, obj))
        print(f"{os.path.basename(f):36s} " + " ".join(f"{t:11.1f}" for t in row))


if __name__ == "__main__":
    main()
