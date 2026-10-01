"""Lazy parsing: fastsimdjson.parse against pysimdjson and cysimdjson.

Three tasks per file:
  parse:  parse the document and look at its root only;
  pick:   parse and extract a few fields (twitter.json, citm_catalog.json);
  walk:   parse and visit every value through the lazy API.
"""
import json
import os
import sys
import time

import fastsimdjson

try:
    import simdjson as pysimdjson
except ImportError:
    pysimdjson = None
try:
    import cysimdjson
except ImportError:
    cysimdjson = None

DATA = os.environ.get("JSONDIR", "simdjson-data/jsonexamples")


def best(fn, data, target=0.02, rounds=21):
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
    return b * 1e6


def walk(v):
    """Visit every value of a lazy document (pysimdjson or fastsimdjson)."""
    if hasattr(v, "items"):
        for _, x in v.items():
            walk(x)
    elif hasattr(v, "__len__") and not isinstance(v, (str, bytes)):
        for x in v:
            walk(x)


def twitter_pick(doc):
    return [(s["id"], s["user"]["screen_name"]) for s in doc["statuses"]]


def citm_pick(doc):
    return [p["start"] for p in doc["performances"]]


PICKS = {"twitter.json": twitter_pick, "citm_catalog.json": citm_pick}


def methods():
    out = {"fastsimdjson.parse": fastsimdjson.parse}
    if pysimdjson is not None:
        p = pysimdjson.Parser()
        out["pysimdjson"] = p.parse
    if cysimdjson is not None:
        c = cysimdjson.JSONParser()
        out["cysimdjson"] = c.parse
    return out


def main():
    files = sys.argv[1:] or ["twitter.json", "citm_catalog.json", "github_events.json",
                             "canada.json", "twitter_api_response.json", "gsoc-2018.json"]
    ms = methods()
    print(f"{'file':28s} {'task':6s} " + " ".join(f"{m:>19s}" for m in ms) + f" {'loads':>9s}")
    for name in files:
        data = open(os.path.join(DATA, name), "rb").read()
        tasks = {"parse": lambda d: None}
        if name in PICKS:
            tasks["pick"] = PICKS[name]
        tasks["walk"] = walk
        for task, fn in tasks.items():
            row = []
            for m, parse in ms.items():
                if m == "cysimdjson" and task != "parse":
                    row.append(float("nan"))  # no Mapping-style API
                    continue
                if task == "pick":
                    assert fn(parse(data)) == fn(json.loads(data)), m
                row.append(best(lambda d: fn(parse(d)), data))
            ref = best(lambda d: fn(fastsimdjson.loads(d)), data)
            print(f"{name:28s} {task:6s} " + " ".join(f"{t:17.1f}us" for t in row) + f" {ref:7.1f}us")


if __name__ == "__main__":
    main()
