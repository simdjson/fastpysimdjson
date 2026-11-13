"""Time minify against parse-and-dump on simdjson-data.

minify removes insignificant whitespace and does not build Python objects.
The other columns parse the document and serialize it again, which is the
usual way to minify JSON in Python. Those paths also rewrite numbers.

Speeds are GB/s of input. The last line is the geometric mean.
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

DATA = os.environ.get("JSONDIR", "simdjson-data/jsonexamples")
FILES = sys.argv[1:] or sorted(glob.glob(os.path.join(DATA, "*.json")))
COMPACT = {"separators": (",", ":"), "ensure_ascii": False}


def best(fn, data, target=0.05, rounds=7):
    fn(data)  # warm up, and fail here if the file cannot be parsed
    n = 1
    while True:
        t0 = time.perf_counter()
        for _ in range(n):
            fn(data)
        if time.perf_counter() - t0 > target:
            break
        n *= 2
    b = float("inf")
    for _ in range(rounds):
        t0 = time.perf_counter()
        for _ in range(n):
            fn(data)
        b = min(b, (time.perf_counter() - t0) / n)
    return b


def json_minify(data):
    return json.dumps(json.loads(data), **COMPACT).encode()


def fast_roundtrip(data):
    return fastsimdjson.dumpb(fastsimdjson.loads(data))


def orjson_minify(data):
    return orjson.dumps(orjson.loads(data))


def main():
    cols = ["minify", "loads+dumpb", "json"]
    fns = [fastsimdjson.minify, fast_roundtrip, json_minify]
    if orjson is not None:
        cols.append("orjson")
        fns.append(orjson_minify)
    print(f"{'file':36s} {'MB':>7s} " + " ".join(f"{c:>12s}" for c in cols) + "   (GB/s)")
    logs = [0.0] * len(cols)
    counted = 0
    for path in FILES:
        data = open(path, "rb").read()
        try:
            got = fastsimdjson.minify(data)
            json.loads(got)
        except Exception as exc:
            print(f"{os.path.basename(path):36s} skipped ({exc})")
            continue
        times = [best(fn, data) for fn in fns]
        mb = len(data) / 1e6
        speeds = [mb / t / 1e3 for t in times]
        for i, s in enumerate(speeds):
            logs[i] += math.log(s)
        counted += 1
        print(f"{os.path.basename(path):36s} {mb:7.2f} " +
              " ".join(f"{s:12.2f}" for s in speeds))
    if counted:
        means = [math.exp(v / counted) for v in logs]
        print(f"{'geometric mean':36s} {'':>7s} " + " ".join(f"{s:12.2f}" for s in means))


if __name__ == "__main__":
    main()
