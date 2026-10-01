"""Time loads_many on NDJSON against parsing each line separately.

The NDJSON input has one compact object or array per line, made from the
simdjson-data files (repeated to reach about 20 MB), or from the file given
on the command line.
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

DATA = os.environ.get("JSONDIR", "simdjson-data/jsonexamples")


def best(fn, data, rounds=7):
    b = float("inf")
    for _ in range(rounds):
        t0 = time.perf_counter()
        fn(data)
        b = min(b, time.perf_counter() - t0)
    return b * 1e3


def make_ndjson():
    docs = []
    for f in sorted(glob.glob(os.path.join(DATA, "*.json"))):
        obj = json.loads(open(f, "rb").read())
        # Split big arrays into many small documents, as in real NDJSON.
        items = obj if isinstance(obj, list) else obj.get("statuses", [obj]) if isinstance(obj, dict) else [obj]
        # Real NDJSON holds objects (or arrays): skip top-level scalars.
        docs.extend(x for x in items if isinstance(x, (dict, list)))
    lines = [json.dumps(d, separators=(",", ":"), ensure_ascii=False) for d in docs]
    text = ("\n".join(lines) + "\n").encode()
    return text * max(1, (20 << 20) // len(text))


def main():
    data = open(sys.argv[1], "rb").read() if len(sys.argv) > 1 else make_ndjson()
    n = data.count(b"\n")
    print(f"{len(data) / 1e6:.1f} MB, {n} documents")
    methods = {
        "json.loads per line": lambda d: [json.loads(l) for l in d.splitlines() if l],
        "fastsimdjson.loads per line": lambda d: [fastsimdjson.loads(l) for l in d.splitlines() if l],
        "fastsimdjson.loads_many": lambda d: list(fastsimdjson.loads_many(d)),
        "fastsimdjson.loads_many(lines)": lambda d: list(fastsimdjson.loads_many(d, format="lines")),
        "fastsimdjson.parse_many": lambda d: [x for x in fastsimdjson.parse_many(d)],
    }
    if orjson:
        methods["orjson.loads per line"] = lambda d: [orjson.loads(l) for l in d.splitlines() if l]
    ref = methods["json.loads per line"](data)
    for name, fn in methods.items():
        if "parse_many" not in name:
            assert fn(data) == ref, name
        t = best(fn, data)
        print(f"{name:34s} {t:9.1f} ms  {len(data) / t / 1e6:6.2f} GB/s")


if __name__ == "__main__":
    main()
