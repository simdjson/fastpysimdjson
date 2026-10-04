"""Time dumps and dumpb against json.dumps, orjson and msgspec on simdjson-data.

Two comparisons, each between functions that produce the same output:
  str:   json.dumps(obj) against fastsimdjson.dumps(obj);
  bytes: compact UTF-8 bytes (no spaces, non-ASCII characters not escaped):
         json.dumps(obj, separators=(",", ":"), ensure_ascii=False).encode(),
         fastsimdjson.dumpb(obj), orjson.dumps(obj) and msgspec.json.encode(obj).
The last line gives geometric means of the speedups.
"""
import glob
import json
import math
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
    cols = ["json str", "fast dumps", "json bytes", "fast dumpb"]
    if orjson:
        cols.append("orjson")
    if msgspec:
        cols.append("msgspec")
    print(f"{'file':36s} " + " ".join(f"{c:>11s}" for c in cols) + "   (us)")
    logs = [0.0] * len(cols)
    for f in FILES:
        obj = json.loads(open(f, "rb").read())
        assert fastsimdjson.dumps(obj) == json.dumps(obj)
        if orjson:
            assert fastsimdjson.dumpb(obj) == orjson.dumps(obj)
        row = [best(json.dumps, obj), best(fastsimdjson.dumps, obj),
               best(lambda o: json.dumps(o, **COMPACT).encode(), obj),
               best(fastsimdjson.dumpb, obj)]
        if orjson:
            row.append(best(orjson.dumps, obj))
        if msgspec:
            row.append(best(msgspec.json.encode, obj))
        for i, t in enumerate(row):
            logs[i] += math.log(t)
        print(f"{os.path.basename(f):36s} " + " ".join(f"{t:11.1f}" for t in row))
    g = [math.exp(x / len(FILES)) for x in logs]
    print(f"dumps is {g[0] / g[1]:.2f}x faster than json.dumps (geometric mean)")
    names = ["fastsimdjson.dumpb"] + ["orjson"] * bool(orjson) + ["msgspec"] * bool(msgspec)
    for i in range(3, len(cols)):
        print(f"{names[i - 3]} is {g[2] / g[i]:.2f}x faster than json.dumps(...).encode(), "
              f"{g[3] / g[i]:.2f}x the speed of dumpb (geometric means)")


if __name__ == "__main__":
    main()
