"""Bar charts for the README, in the style of simdjson's (light and dark).

The numbers are those of the README's benchmark section. Run from the
repository root: python doc/make_charts.py (needs matplotlib), or
python doc/make_charts.py arrow (msgpack, typed) for the Arrow (MessagePack,
typed decoding) charts only.
"""
import os
import sys

import matplotlib

matplotlib.use("Agg")
import matplotlib.pyplot as plt
import matplotlib.ticker
from matplotlib import font_manager

HERE = os.path.dirname(os.path.abspath(__file__))
ONLY = sys.argv[1] if len(sys.argv) > 1 else None  # "arrow", "msgpack", "minify" or None
MACHINE = "Intel Xeon Gold 6548N (Emerald Rapids)"
PYTHON = "Python 3.14.10"

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


META = f"fastsimdjson 2.0.1  ·  {PYTHON}  ·  GCC 16.2  ·  {MACHINE}"

chart("perf_loads",
      "Parsing JSON in Python",
      "the 22 files of simdjson-data into Python objects (geometric mean)",
      META,
      [("fastsimdjson", "loads", 0.77, True),
       ("orjson", "3.13.0", 0.60, False),
       ("jiter", "0.19.0, string cache", 0.54, False),
       ("msgspec", "0.22.0", 0.53, False),
       ("jiter", "0.19.0, no string cache", 0.44, False),
       ("pysimdjson", "8.0.0", 0.44, False),
       ("cysimdjson", "26.27", 0.43, False),
       ("ujson", "6.1.0", 0.37, False),
       ("python-rapidjson", "1.26", 0.24, False),
       ("simplejson", "4.3.0", 0.23, False),
       ("json", "standard library", 0.22, False)],
      "Parsing speed", "GB/s", " GB/s")

chart("perf_parse",
      "Reading part of a document",
      "the id and screen name of the 100 statuses of twitter.json",
      META,
      [("fastsimdjson", "parse (lazy)", 156, True),
       ("pysimdjson", "lazy", 170, False),
       ("cysimdjson", "lazy", 235, False),
       ("msgspec", "typed Struct", 336, False),
       ("fastsimdjson", "loads", 843, True),
       ("orjson", "loads", 981, False),
       ("jiter", "string cache", 1148, False),
       ("jiter", "no string cache", 1580, False),
       ("json", "loads", 3977, False)],
      "Time", "µs", " µs", fmt="{:.0f}", lower_is_better=True)

chart("perf_dumpb",
      "Writing JSON as bytes",
      "compact UTF-8 bytes from the 22 files of simdjson-data (geometric mean)",
      META,
      [("fastsimdjson", "dumpb", 8.03, True),
       ("orjson", "3.13.0", 7.39, False),
       ("msgspec", "0.22.0", 5.26, False),
       ("json", "dumps(...).encode()", 1.00, False)],
      "Speed relative to json.dumps(...).encode()", "", "×")

chart("perf_dumps",
      "Writing JSON as str",
      "the same str as json.dumps, from the 22 files of simdjson-data (geometric mean)",
      META,
      [("fastsimdjson", "dumps", 4.26, True),
       ("json", "dumps", 1.00, False)],
      "Speed relative to json.dumps", "", "×")

chart("perf_ndjson",
      "Reading NDJSON",
      "20 MB, 5268 documents, one per line",
      META,
      [("fastsimdjson", "parse_many (views)", 1.14, True),
       ("fastsimdjson", "loads_many", 0.40, True),
       ("msgspec", "decode_lines", 0.36, False),
       ("fastsimdjson", "loads on each line", 0.34, True),
       ("orjson", "loads on each line", 0.27, False),
       ("msgspec", "decode on each line", 0.27, False),
       ("jiter", "each line, string cache", 0.24, False),
       ("jiter", "each line, no string cache", 0.21, False),
       ("json", "loads on each line", 0.12, False)],
      "Parsing speed", "GB/s", " GB/s")

# NDJSON to Arrow and back: bench_arrow.py, Python 3.14, the result as a
# pyarrow.Table (reading) or bytes (writing).
META_ARROW = ("fastsimdjson 2.0.1  ·  pyarrow 26.0  ·  polars 2.1  ·  DuckDB 2.0  ·  "
              "Python 3.14.10  ·  Xeon Gold 6548N")


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


arrow_read(1, (0.79, 0.18, 0.27), (1.98, 0.46, 0.54), (0.78, 0.18, 0.09))
arrow_read(32, (14.7, 5.2, 4.0), (28.6, 12.3, 3.9), (14.0, 5.0, 0.12))
arrow_write(1, (0.92, 0.45, 0.32), (1.07, 0.46, 0.32))
arrow_write(32, (9.5, 1.5, 4.9), (4.6, 1.0, 1.2))

# JSON to MessagePack and back: bench_msgpack.py, bytes in, bytes out, on one
# core (the others decode, then encode).
META_MSGPACK = ("fastsimdjson 2.0.1  ·  msgpack 1.2  ·  orjson 3.13  ·  ormsgpack 1.13  ·  "
                "msgspec 0.22  ·  Python 3.14  ·  Xeon Gold 6548N")

chart("perf_msgpack_encode",
      "JSON to MessagePack in Python",
      "the 22 files of simdjson-data, from JSON bytes to MessagePack bytes (geometric mean)",
      META_MSGPACK,
      [("fastsimdjson", "json_to_msgpack", 1.67, True),
       ("orjson + ormsgpack", "ormsgpack.packb(orjson.loads(s))", 0.46, False),
       ("msgspec", "msgpack.encode(json.decode(s))", 0.42, False),
       ("fastsimdjson + msgpack", "msgpack.packb(fastsimdjson.loads(s))", 0.34, False),
       ("orjson + msgpack", "msgpack.packb(orjson.loads(s))", 0.32, False),
       ("json + msgpack", "msgpack.packb(json.loads(s))", 0.17, False)],
      "Speed", "GB/s of JSON", " GB/s", kind="msgpack", left=0.37)

chart("perf_msgpack_decode",
      "MessagePack to JSON in Python",
      "the 22 files of simdjson-data, from MessagePack bytes to JSON bytes (geometric mean)",
      META_MSGPACK,
      [("fastsimdjson", "msgpack_to_json", 1.43, True),
       ("ormsgpack + orjson", "orjson.dumps(ormsgpack.unpackb(b))", 0.34, False),
       ("msgspec", "json.encode(msgpack.decode(b))", 0.32, False),
       ("msgpack + fastsimdjson", "fastsimdjson.dumpb(msgpack.unpackb(b))", 0.26, False),
       ("msgpack + orjson", "orjson.dumps(msgpack.unpackb(b))", 0.26, False),
       ("msgpack + json", "json.dumps(msgpack.unpackb(b)).encode()", 0.10, False)],
      "Speed", "GB/s of JSON", " GB/s", kind="msgpack", left=0.37)

grouped_chart("perf_msgpack_ndjson",
              "NDJSON and MessagePack streams",
              "GH Archive (886 MB, 304,929 events), on several threads",
              META_MSGPACK,
              [("NDJSON to MessagePack",
                [("fastsimdjson", "64 threads", 22.2, True),
                 ("fastsimdjson", "32 threads", 16.1, True),
                 ("fastsimdjson", "8 threads", 8.7, True),
                 ("fastsimdjson", "one thread", 1.32, True),
                 ("orjson + ormsgpack", "each document, one thread", 0.36, False)]),
               ("MessagePack to NDJSON",
                [("fastsimdjson", "8 threads", 6.0, True),
                 ("fastsimdjson", "one thread", 1.51, True),
                 ("msgpack + orjson", "Unpacker, each value, one thread", 0.25, False)])],
              "Speed", kind="msgpack", left=0.33)



def summary_chart(name, title, subtitle, meta, rows, kind="summary", left=0.33):
    """rows: (task, call, speedup, alternative) from top to bottom; the alternative is 1x."""
    if ONLY is not None and kind != ONLY:
        return
    font = pick_font()
    n = len(rows)
    top = 1.45
    pitch = 0.78  # inches per task
    height = top + pitch * n + 0.95
    vmax = max(r[2] for r in rows)
    for theme, c in THEMES.items():
        plt.rcParams["font.family"] = font
        fig = plt.figure(figsize=(10, height), dpi=200, facecolor=c["bg"])
        right = 0.955
        ax = fig.add_axes([left, 0.9 / height, right - left, (pitch * n) / height], facecolor=c["bg"])
        for y, (task, call, speedup, alternative) in zip(list(range(n))[::-1], rows):
            ax.barh(y + 0.19, speedup, height=0.34, color=c["ours"], zorder=3)
            ax.barh(y - 0.19, 1, height=0.34, color=c["bar"], zorder=3)
            ax.text(speedup + vmax * 0.012, y + 0.19, f"{speedup:g}×", va="center", ha="left",
                    fontsize=15, fontweight="bold", color=c["value_ours"], zorder=4)
            ax.text(1 + vmax * 0.012, y - 0.19, alternative, va="center", ha="left",
                    fontsize=12, color=c["value"], zorder=4)
            ax.text(-vmax * 0.022, y + 0.13, task, va="center", ha="right", fontsize=14.5,
                    color=c["text"], clip_on=False)
            ax.text(-vmax * 0.022, y - 0.24, call, va="center", ha="right", fontsize=11.5,
                    color=c["meta"], clip_on=False)
        ax.set_xlim(0, vmax * 1.12)
        ax.set_ylim(-0.6, n - 0.4)
        ax.set_yticks([])
        ax.set_xticks(range(0, int(vmax) + 1))
        ax.tick_params(axis="x", colors=c["meta"], labelsize=12, length=0, pad=8)
        ax.grid(axis="x", color=c["grid"], linewidth=1, zorder=0)
        for side in ("top", "right", "left"):
            ax.spines[side].set_visible(False)
        ax.spines["bottom"].set_color(c["axis"])
        ax.set_xlabel("Speed relative to the alternative (higher is better)",
                      color=c["sub"], fontsize=13, labelpad=10)
        ax.xaxis.set_major_formatter(matplotlib.ticker.FuncFormatter(lambda x, _: f"{x:g}×"))
        fig.text(0.04, 1 - 0.42 / height, title, fontsize=25, color=c["text"], va="center")
        fig.text(0.04, 1 - 0.85 / height, subtitle, fontsize=14, color=c["sub"], va="center")
        fig.text(0.04, 1 - 1.15 / height, meta, fontsize=12.5, color=c["meta"], va="center")
        suffix = "" if theme == "light" else "_dark"
        fig.savefig(os.path.join(HERE, f"{name}{suffix}.png"), facecolor=c["bg"])
        plt.close(fig)


# The README's summary: each task against a fast alternative (1x), one core.
summary_chart("perf_summary",
              "fastsimdjson against the competition",
              "speed on one core, relative to a fast Python alternative for each task (1×)",
              "fastsimdjson 2.0.1  ·  Python 3.14.10  ·  GCC 16.2  ·  Intel Xeon Gold 6548N",
              [("Python objects to JSON str", "dumps, same str as json.dumps", 4.3, "json.dumps"),
               ("MessagePack to JSON", "msgpack_to_json", 4.2, "ormsgpack + orjson"),
               ("JSON to MessagePack", "json_to_msgpack", 3.6, "orjson + ormsgpack"),
               ("NDJSON to Arrow", "loads_arrow, GH Archive", 3.6, "polars"),
               ("Arrow to NDJSON", "dumpb_arrow, GH Archive", 2.4, "polars"),
               ("A few fields of a document", "parse (lazy)", 2.2, "msgspec, typed Struct"),
               ("JSON to Python objects", "loads", 1.3, "orjson")])


META_TYPED = ("fastsimdjson 2.0.1  ·  msgspec 0.22  ·  pydantic 2.14  ·  cattrs 26.2  ·  "
              "Python 3.14.10  ·  Xeon Gold 6548N")


def typed_rows(ours, struct, dataclass, pydantic, cattrs, call="loads(data, type=T)"):
    return [("fastsimdjson", call, ours, True),
            ("msgspec", "Decoder(T), msgspec.Struct", struct, False),
            ("msgspec", "Decoder(T), the same dataclasses", dataclass, False),
            ("pydantic", "TypeAdapter(T).validate_json", pydantic, False),
            ("orjson + cattrs", "structure(orjson.loads(data), T)", cattrs, False)]


grouped_chart("perf_typed",
              "JSON to dataclasses",
              "typed decoding, with the types checked, on one core",
              META_TYPED,
              [("twitter.json, a few fields", typed_rows(2.59, 1.53, 1.43, 0.66, 0.57)),
               ("citm_catalog.json, typed in full", typed_rows(1.00, 0.64, 0.47, 0.19, 0.19)),
               ("GH Archive NDJSON, 6 fields", typed_rows(2.05, 1.29, 1.16, 0.52, 0.42,
                                                                 "loads_many(data, type=T)"))],
              "Speed", kind="typed", left=0.36)

# minify against parsing and serializing again: bench_minify.py, one core.
chart("perf_minify",
      "Minifying JSON in Python",
      "the 22 files of simdjson-data, from JSON bytes to compact JSON bytes (geometric mean)",
      "fastsimdjson 2.0.1  ·  orjson 3.13  ·  Python 3.14.10  ·  GCC 16.2  ·  Intel Xeon Gold 6548N",
      [("fastsimdjson", "minify", 18.58, True),
       ("fastsimdjson", "dumpb(loads(s))", 0.54, True),
       ("orjson", "dumps(loads(s))", 0.44, False),
       ("json", "dumps(loads(s))", 0.11, False)],
      "Speed", "GB/s of input", " GB/s", kind="minify", left=0.25)
