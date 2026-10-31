"""Time json_to_msgpack and msgpack_to_json against the usual ways to convert
JSON to MessagePack and back in Python, on simdjson-data.

Each way decodes, then encodes:
  JSON to MessagePack: msgpack.packb(json.loads(s)), msgpack.packb(orjson.loads(s)),
      ormsgpack.packb(orjson.loads(s)), msgspec.msgpack.encode(msgspec.json.decode(s)),
      msgpack.packb(fastsimdjson.loads(s)), and fastsimdjson.json_to_msgpack(s);
  MessagePack to JSON: json.dumps(msgpack.unpackb(b)) (compact, then encoded),
      orjson.dumps(msgpack.unpackb(b)), orjson.dumps(ormsgpack.unpackb(b)),
      msgspec.json.encode(msgspec.msgpack.decode(b)), and fastsimdjson.msgpack_to_json(b).
We check that the MessagePack of the others decodes to the same values as ours.
Times in microseconds; the last lines give, for each way, the geometric mean of
its speed in GB/s of JSON (read, or written) over the files.

  python bench_msgpack.py [files...]        (JSONDIR: the directory of the files)
  python bench_msgpack.py --ndjson file.ndjson [threads...]
"""
import glob
import json
import math
import os
import sys
import time

import fastsimdjson
import msgpack

try:
    import orjson
except ImportError:
    orjson = None
try:
    import ormsgpack
except ImportError:
    ormsgpack = None
try:
    import msgspec
except ImportError:
    msgspec = None

DATA = os.environ.get("JSONDIR", "simdjson-data/jsonexamples")
COMPACT = {"separators": (",", ":"), "ensure_ascii": False}


def best(fn, arg, target=0.05, rounds=11):
    n = 1
    while True:
        t0 = time.perf_counter()
        for _ in range(n):
            fn(arg)
        if time.perf_counter() - t0 > target:
            break
        n *= 2
    b = float("inf")
    for _ in range(rounds):
        t0 = time.perf_counter()
        for _ in range(n):
            fn(arg)
        b = min(b, (time.perf_counter() - t0) / n)
    return b


def to_msgpack_ways():
    ways = [("fastsimdjson", fastsimdjson.json_to_msgpack),
            ("json + msgpack", lambda s: msgpack.packb(json.loads(s)))]
    if orjson:
        ways.append(("orjson + msgpack", lambda s: msgpack.packb(orjson.loads(s))))
        if ormsgpack:
            ways.append(("orjson + ormsgpack", lambda s: ormsgpack.packb(orjson.loads(s))))
    if msgspec:
        ways.append(("msgspec", lambda s, d=msgspec.json.Decoder(), e=msgspec.msgpack.Encoder():
                     e.encode(d.decode(s))))
    ways.append(("fastsimdjson.loads + msgpack", lambda s: msgpack.packb(fastsimdjson.loads(s))))
    return ways


def to_json_ways():
    ways = [("fastsimdjson", fastsimdjson.msgpack_to_json),
            ("msgpack + json", lambda b: json.dumps(msgpack.unpackb(b), **COMPACT).encode())]
    if orjson:
        ways.append(("msgpack + orjson", lambda b: orjson.dumps(msgpack.unpackb(b))))
        if ormsgpack:
            ways.append(("ormsgpack + orjson", lambda b: orjson.dumps(ormsgpack.unpackb(b))))
    if msgspec:
        ways.append(("msgspec", lambda b, d=msgspec.msgpack.Decoder(), e=msgspec.json.Encoder():
                     e.encode(d.decode(b))))
    ways.append(("msgpack + fastsimdjson.dumpb",
                 lambda b: fastsimdjson.dumpb(msgpack.unpackb(b))))
    return ways


def usable(path):
    """Files that every way converts: no integer beyond 64 bits, no duplicate key."""
    def pairs(p):
        keys = [k for k, _ in p]
        if len(keys) != len(set(keys)):
            raise ValueError("duplicate key")
        return dict(p)
    try:
        value = json.loads(open(path, "rb").read(), object_pairs_hook=pairs)
        msgpack.packb(value)
        return True
    except (ValueError, OverflowError, UnicodeDecodeError):
        return False


def files_benchmark(files):
    ways1, ways2 = to_msgpack_ways(), to_json_ways()
    logs1, logs2 = [0.0] * len(ways1), [0.0] * len(ways2)
    count = 0
    print(f"{'JSON to MessagePack (us)':28s} " + " ".join(f"{n[:16]:>16s}" for n, _ in ways1))
    rows2 = []
    for f in files:
        data = open(f, "rb").read()
        ours = fastsimdjson.json_to_msgpack(data)
        expected = msgpack.unpackb(ours, strict_map_key=False)
        for name, fn in ways1:
            assert msgpack.unpackb(fn(data), strict_map_key=False) == expected, (name, f)
        t1 = [best(fn, data) for _, fn in ways1]
        out = fastsimdjson.msgpack_to_json(ours)
        t2 = [best(fn, ours) for _, fn in ways2]
        for i, t in enumerate(t1):
            logs1[i] += math.log(len(data) / t / 1e9)
        for i, t in enumerate(t2):
            logs2[i] += math.log(len(out) / t / 1e9)
        count += 1
        name = os.path.basename(f)
        print(f"{name:28s} " + " ".join(f"{t * 1e6:16.1f}" for t in t1))
        rows2.append((name, t2))
    print(f"{'MessagePack to JSON (us)':28s} " + " ".join(f"{n[:16]:>16s}" for n, _ in ways2))
    for name, t2 in rows2:
        print(f"{name:28s} " + " ".join(f"{t * 1e6:16.1f}" for t in t2))
    print(f"\nGeometric means over {count} files, in GB/s of JSON:")
    print("JSON to MessagePack: " + ", ".join(
        f"{n} {math.exp(l / count):.3f}" for (n, _), l in zip(ways1, logs1)))
    print("MessagePack to JSON: " + ", ".join(
        f"{n} {math.exp(l / count):.3f}" for (n, _), l in zip(ways2, logs2)))


def ndjson_benchmark(path, threads):
    data = open(path, "rb").read()
    packed = fastsimdjson.json_to_msgpack(data, ndjson=True)
    lines = [line for line in data.splitlines() if line.strip()]
    print(f"{path}: {len(data) / 1e6:.0f} MB of NDJSON, {len(lines)} documents")
    for t in threads:
        a = best(lambda d: fastsimdjson.json_to_msgpack(d, ndjson=True, threads=t), data,
                 rounds=5)
        b = best(lambda d: fastsimdjson.msgpack_to_json(d, stream=True, threads=t), packed,
                 rounds=5)
        print(f"threads {t:3d}: JSON to MessagePack {len(data) / a / 1e9:6.2f} GB/s, "
              f"MessagePack to JSON {len(data) / b / 1e9:6.2f} GB/s")
    if orjson and ormsgpack:
        # The fastest of the others, on one thread: a document at a time.
        a = best(lambda ls: b"".join(ormsgpack.packb(orjson.loads(x)) for x in ls), lines,
                 rounds=3)
        print(f"orjson + ormsgpack, one thread: JSON to MessagePack {len(data) / a / 1e9:6.2f} GB/s")

        def to_ndjson(b):
            unpacker = msgpack.Unpacker(max_buffer_size=len(b), strict_map_key=False)
            unpacker.feed(b)
            return b"".join(orjson.dumps(v) + b"\n" for v in unpacker)
        b = best(to_ndjson, packed, rounds=3)
        print(f"msgpack.Unpacker + orjson, one thread: MessagePack to JSON "
              f"{len(data) / b / 1e9:6.2f} GB/s")

def main():
    args = sys.argv[1:]
    if args and args[0] == "--ndjson":
        ndjson_benchmark(args[1], [int(t) for t in args[2:]] or [1])
        return
    files = args or sorted(glob.glob(os.path.join(DATA, "*.json")))
    files = [f for f in files if usable(f)]
    files_benchmark(files)


if __name__ == "__main__":
    main()
