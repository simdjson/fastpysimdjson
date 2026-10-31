"""Bar charts for the README, in the style of simdjson's (light and dark).

The numbers are those of the README's benchmark section. Run from the
repository root: python doc/make_charts.py (needs matplotlib), or
python doc/make_charts.py arrow (msgpack) for the Arrow (MessagePack) charts
only.
"""
import os
import sys

import matplotlib

matplotlib.use("Agg")
import matplotlib.pyplot as plt
import matplotlib.ticker
from matplotlib import font_manager

HERE = os.path.dirname(os.path.abspath(__file__))
ONLY = sys.argv[1] if len(sys.argv) > 1 else None  # "arrow", "msgpack" or None
MACHINE = "Intel Xeon Gold 6548N (Emerald Rapids)"
PYTHON = "Python 3.14.6"

THEMES = {
    "light": dict(bg="#ffffff", text="#111111", sub="#555555", meta="#8a8a8a", grid="#e2e2e2",
                  axis="#d0d0d0", bar="#c5c4bf", ours="#2a78d6", value="#333333", value_ours="#111111"),
    "dark": dict(bg="#0d1117", text="#f0f0f0", sub="#c9c9c9", meta="#9a9a9a", grid="#30343b",
                 axis="#3d424a", bar="#4a4e57", ours="#3a8ae6", value="#d0d0d0", value_ours="#f0f0f0"),
}


def pick_font():
    available = {f.name for f in font_manager.fontManager.ttflist}
    for name in ("Helvetica Neue", "Helvetica", "Arial", "Liberation Sans", "DejaVu Sans"):
        if name in available:
            return name
    return "sans-serif"


def chart(name, title, subtitle, meta, rows, xlabel, xunit, unit, fmt="{:.2f}", lower_is_better=False,
          kind=None, left=0.205):
    """rows: (label, sublabel, value, ours) from top to bottom."""
    if ONLY is not None and kind != ONLY:
        return
    font = pick_font()
    n = len(rows)
    top = 1.45  # inches above the plot for the titles
    height = top + 0.58 * n + 0.95
    for theme, c in THEMES.items():
        plt.rcParams["font.family"] = font
        fig = plt.figure(figsize=(10, height), dpi=200, facecolor=c["bg"])
        right = 0.955
        ax = fig.add_axes([left, 0.9 / height, right - left, (0.58 * n) / height], facecolor=c["bg"])
        vmax = max(r[2] for r in rows)
        ys = list(range(n))[::-1]
        for y, (label, sub, value, ours) in zip(ys, rows):
            ax.barh(y, value, height=0.62, color=c["ours"] if ours else c["bar"], zorder=3)
            ax.text(value + vmax * 0.012, y, fmt.format(value) + unit, va="center", ha="left",
                    fontsize=14.5, color=c["value_ours"] if ours else c["value"], zorder=4)
            ax.text(-vmax * 0.022, y + (0.13 if sub else 0), label, va="center", ha="right",
                    fontsize=15, color=c["text"], transform=ax.transData, clip_on=False)
            if sub:
                ax.text(-vmax * 0.022, y - 0.27, sub, va="center", ha="right", fontsize=11.5,
                        color=c["meta"], transform=ax.transData, clip_on=False)
        ax.set_xlim(0, vmax * 1.17)
        ax.set_ylim(-0.6, n - 0.4)
        ax.set_yticks([])
        ax.tick_params(axis="x", colors=c["meta"], labelsize=12, length=0, pad=8)
        ax.grid(axis="x", color=c["grid"], linewidth=1, zorder=0)
        for side in ("top", "right", "left"):
            ax.spines[side].set_visible(False)
        ax.spines["bottom"].set_color(c["axis"])
        better = "lower is better" if lower_is_better else "higher is better"
        ax.set_xlabel(f"{xlabel} ({xunit + ', ' if xunit else ''}{better})", color=c["sub"],
                      fontsize=13, labelpad=10)
        ax.xaxis.set_major_formatter(matplotlib.ticker.FuncFormatter(lambda x, _: f"{x:g}"))
        fig.text(0.04, 1 - 0.42 / height, title, fontsize=25, color=c["text"], va="center")
        fig.text(0.04, 1 - 0.85 / height, subtitle, fontsize=14, color=c["sub"], va="center")
        fig.text(0.04, 1 - 1.15 / height, meta, fontsize=12.5, color=c["meta"], va="center")
        suffix = "" if theme == "light" else "_dark"
        fig.savefig(os.path.join(HERE, f"{name}{suffix}.png"), facecolor=c["bg"])
        plt.close(fig)


META = f"fastsimdjson 0.3.0  ·  {PYTHON}  ·  {MACHINE}"

chart("perf_loads",
      "Parsing JSON in Python",
      "the 22 files of simdjson-data into Python objects (geometric mean)",
      META,
      [("fastsimdjson", "loads", 0.78, True),
       ("orjson", "3.12.0", 0.60, False),
       ("msgspec", "0.22.0", 0.53, False),
       ("jiter", "0.17.0, string cache", 0.48, False),
       ("jiter", "0.17.0, no string cache", 0.44, False),
       ("pysimdjson", "7.0.2", 0.44, False),
       ("cysimdjson", "26.27", 0.43, False),
       ("ujson", "6.0.0", 0.37, False),
       ("python-rapidjson", "1.25", 0.24, False),
       ("simplejson", "4.2.0", 0.23, False),
       ("json", "standard library", 0.22, False)],
      "Parsing speed", "GB/s", " GB/s")

chart("perf_parse",
      "Reading part of a document",
      "the id and screen name of the 100 statuses of twitter.json",
      META,
      [("fastsimdjson", "parse (lazy)", 158, True),
       ("pysimdjson", "lazy", 179, False),
       ("cysimdjson", "lazy", 229, False),
       ("msgspec", "typed Struct", 336, False),
       ("fastsimdjson", "loads", 861, True),
       ("orjson", "loads", 1009, False),
       ("jiter", "string cache", 1281, False),
       ("jiter", "no string cache", 1592, False),
       ("json", "loads", 3922, False)],
      "Time", "µs", " µs", fmt="{:.0f}", lower_is_better=True)

chart("perf_dumpb",
      "Writing JSON as bytes",
      "compact UTF-8 bytes from the 22 files of simdjson-data (geometric mean)",
      META,
      [("fastsimdjson", "dumpb", 10.09, True),
       ("orjson", "3.12.0", 9.69, False),
       ("msgspec", "0.22.0", 6.03, False),
       ("json", "dumps(...).encode()", 1.00, False)],
      "Speed relative to json.dumps(...).encode()", "", "×")

chart("perf_dumps",
      "Writing JSON as str",
      "the same str as json.dumps, from the 22 files of simdjson-data (geometric mean)",
      META,
      [("fastsimdjson", "dumps", 4.63, True),
       ("json", "dumps", 1.00, False)],
      "Speed relative to json.dumps", "", "×")

chart("perf_ndjson",
      "Reading NDJSON",
      "20 MB, 5268 documents, one per line",
      META,
      [("fastsimdjson", "parse_many (views)", 0.94, True),
       ("fastsimdjson", "loads_many", 0.38, True),
       ("msgspec", "decode_lines", 0.37, False),
       ("fastsimdjson", "loads on each line", 0.33, True),
       ("msgspec", "decode on each line", 0.27, False),
       ("orjson", "loads on each line", 0.26, False),
       ("jiter", "each line, string cache", 0.22, False),
       ("jiter", "each line, no string cache", 0.20, False),
       ("json", "loads on each line", 0.12, False)],
      "Parsing speed", "GB/s", " GB/s")

# NDJSON to Arrow and back: bench_arrow.py, Python 3.13, the result as a
# pyarrow.Table (reading) or bytes (writing).
META_ARROW = ("fastsimdjson 0.10.0  ·  pyarrow 26.0  ·  polars 2.1  ·  DuckDB 2.0  ·  "
              "Python 3.13  ·  Xeon Gold 6548N")


def gbps(v):
    """The value as given (we pass rounded numbers): 0.12, 0.20, 1.95, 4.0, 13.9."""
    if v < 1:
        return f"{v:.2f}"
    text = f"{v:g}"
    return text if "." in text else text + ".0"


def grouped_chart(name, title, subtitle, meta, groups, xlabel, kind="arrow", left=0.235):
    """groups: (heading, [(label, sublabel, value, ours)]) from top to bottom."""
    if ONLY is not None and kind != ONLY:
        return
    font = pick_font()
    slots = []  # (kind, payload) from top to bottom
    for heading, rows in groups:
        slots.append(("heading", heading))
        slots.extend(("bar", r) for r in rows)
    n = len(slots)
    top = 1.45
    height = top + 0.5 * n + 0.95
    vmax = max(r[2] for _, rows in groups for r in rows)
    for theme, c in THEMES.items():
        plt.rcParams["font.family"] = font
        fig = plt.figure(figsize=(10, height), dpi=200, facecolor=c["bg"])
        right = 0.955
        ax = fig.add_axes([left, 0.9 / height, right - left, (0.5 * n) / height], facecolor=c["bg"])
        for y, (kind, payload) in zip(list(range(n))[::-1], slots):
            if kind == "heading":
                ax.text(-vmax * 0.022, y - 0.05, payload, va="center", ha="right", fontsize=15,
                        fontweight="bold", color=c["text"], clip_on=False)
                continue
            label, sub, value, ours = payload
            ax.barh(y, value, height=0.66, color=c["ours"] if ours else c["bar"], zorder=3)
            ax.text(value + vmax * 0.012, y, gbps(value) + " GB/s", va="center", ha="left",
                    fontsize=14, color=c["value_ours"] if ours else c["value"], zorder=4)
            ax.text(-vmax * 0.022, y + 0.12, label, va="center", ha="right", fontsize=14,
                    color=c["text"], clip_on=False)
            ax.text(-vmax * 0.022, y - 0.26, sub, va="center", ha="right", fontsize=11,
                    color=c["meta"], clip_on=False)
        ax.set_xlim(0, vmax * 1.2)
        ax.set_ylim(-0.6, n - 0.4)
        ax.set_yticks([])
        ax.tick_params(axis="x", colors=c["meta"], labelsize=12, length=0, pad=8)
        ax.grid(axis="x", color=c["grid"], linewidth=1, zorder=0)
        for side in ("top", "right", "left"):
            ax.spines[side].set_visible(False)
        ax.spines["bottom"].set_color(c["axis"])
        ax.set_xlabel(f"{xlabel} (GB/s, higher is better)", color=c["sub"], fontsize=13,
                      labelpad=10)
        ax.xaxis.set_major_formatter(matplotlib.ticker.FuncFormatter(lambda x, _: f"{x:g}"))
        fig.text(0.04, 1 - 0.42 / height, title, fontsize=25, color=c["text"], va="center")
        fig.text(0.04, 1 - 0.85 / height, subtitle, fontsize=14, color=c["sub"], va="center")
        fig.text(0.04, 1 - 1.15 / height, meta, fontsize=12.5, color=c["meta"], va="center")
        suffix = "" if theme == "light" else "_dark"
        fig.savefig(os.path.join(HERE, f"{name}{suffix}.png"), facecolor=c["bg"])
        plt.close(fig)


def threads_text(threads):
    return "one thread" if threads == 1 else f"{threads} threads"


def arrow_read(threads, flat, gh, inferred):
    """Each argument but threads: (fastsimdjson, pyarrow, polars) in GB/s."""
    who = (("fastsimdjson", "loads_arrow"), ("pyarrow", "json.read_json"), ("polars", "read_ndjson"))
    groups = [(heading, [(w, call, v, w == "fastsimdjson") for (w, call), v in zip(who, values)])
              for heading, values in (("flat", flat), ("GH Archive", gh),
                                      ("flat, inferred schema", inferred))]
    grouped_chart(f"perf_arrow_read_{threads}", "Reading NDJSON into Arrow",
                  f"into a pyarrow.Table, on {threads_text(threads)}", META_ARROW, groups,
                  "Reading speed")


def arrow_write(threads, flat, gh):
    """Each argument but threads: (fastsimdjson, polars, DuckDB) in GB/s."""
    who = (("fastsimdjson", "dumpb_arrow"), ("polars", "write_ndjson"),
           ("DuckDB", "COPY ... (FORMAT json)"))
    groups = [(heading, [(w, call, v, w == "fastsimdjson") for (w, call), v in zip(who, values)])
              for heading, values in (("flat", flat), ("GH Archive", gh))]
    grouped_chart(f"perf_arrow_write_{threads}", "Writing Arrow as NDJSON",
                  f"from a pyarrow.Table, on {threads_text(threads)}", META_ARROW, groups,
                  "Writing speed")


arrow_read(1, (0.70, 0.18, 0.27), (1.95, 0.45, 0.53), (0.72, 0.17, 0.08))
arrow_read(32, (13.9, 5.1, 4.0), (28.4, 12.1, 3.8), (13.3, 4.8, 0.12))
arrow_write(1, (0.96, 0.46, 0.30), (1.07, 0.47, 0.31))
arrow_write(32, (7.8, 1.3, 2.8), (4.7, 1.1, 0.79))

# JSON to MessagePack and back: bench_msgpack.py, bytes in, bytes out, on one
# core (the others decode, then encode).
META_MSGPACK = ("fastsimdjson 0.10.0  ·  msgpack 1.2  ·  orjson 3.13  ·  ormsgpack 1.13  ·  "
                "msgspec 0.22  ·  Python 3.14  ·  Xeon Gold 6338")

chart("perf_msgpack_encode",
      "JSON to MessagePack in Python",
      "the 22 files of simdjson-data, from JSON bytes to MessagePack bytes (geometric mean)",
      META_MSGPACK,
      [("fastsimdjson", "json_to_msgpack", 1.37, True),
       ("orjson + ormsgpack", "ormsgpack.packb(orjson.loads(s))", 0.35, False),
       ("msgspec", "msgpack.encode(json.decode(s))", 0.34, False),
       ("fastsimdjson + msgpack", "msgpack.packb(fastsimdjson.loads(s))", 0.28, False),
       ("orjson + msgpack", "msgpack.packb(orjson.loads(s))", 0.25, False),
       ("json + msgpack", "msgpack.packb(json.loads(s))", 0.13, False)],
      "Speed", "GB/s of JSON", " GB/s", kind="msgpack", left=0.37)

chart("perf_msgpack_decode",
      "MessagePack to JSON in Python",
      "the 22 files of simdjson-data, from MessagePack bytes to JSON bytes (geometric mean)",
      META_MSGPACK,
      [("fastsimdjson", "msgpack_to_json", 0.94, True),
       ("ormsgpack + orjson", "orjson.dumps(ormsgpack.unpackb(b))", 0.27, False),
       ("msgspec", "json.encode(msgpack.decode(b))", 0.25, False),
       ("msgpack + orjson", "orjson.dumps(msgpack.unpackb(b))", 0.20, False),
       ("msgpack + fastsimdjson", "fastsimdjson.dumpb(msgpack.unpackb(b))", 0.20, False),
       ("msgpack + json", "json.dumps(msgpack.unpackb(b)).encode()", 0.07, False)],
      "Speed", "GB/s of JSON", " GB/s", kind="msgpack", left=0.37)

grouped_chart("perf_msgpack_ndjson",
              "NDJSON and MessagePack streams",
              "GH Archive (886 MB, 304,929 events), on several threads",
              META_MSGPACK,
              [("NDJSON to MessagePack",
                [("fastsimdjson", "64 threads", 15.0, True),
                 ("fastsimdjson", "32 threads", 10.8, True),
                 ("fastsimdjson", "8 threads", 6.8, True),
                 ("fastsimdjson", "one thread", 1.18, True),
                 ("orjson + ormsgpack", "each document, one thread", 0.29, False)]),
               ("MessagePack to NDJSON",
                [("fastsimdjson", "8 threads", 5.0, True),
                 ("fastsimdjson", "one thread", 1.05, True),
                 ("msgpack + orjson", "Unpacker, each value, one thread", 0.20, False)])],
              "Speed", kind="msgpack", left=0.33)

