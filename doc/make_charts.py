"""Bar charts for the README, in the style of simdjson's (light and dark).

The numbers are those of the README's benchmark section. Run from the
repository root: python doc/make_charts.py (needs matplotlib).
"""
import os

import matplotlib

matplotlib.use("Agg")
import matplotlib.pyplot as plt
import matplotlib.ticker
from matplotlib import font_manager

HERE = os.path.dirname(os.path.abspath(__file__))
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


def chart(name, title, subtitle, meta, rows, xlabel, xunit, unit, fmt="{:.2f}", lower_is_better=False):
    """rows: (label, sublabel, value, ours) from top to bottom."""
    font = pick_font()
    n = len(rows)
    top = 1.45  # inches above the plot for the titles
    height = top + 0.58 * n + 0.95
    for theme, c in THEMES.items():
        plt.rcParams["font.family"] = font
        fig = plt.figure(figsize=(10, height), dpi=200, facecolor=c["bg"])
        left, right = 0.205, 0.955
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
       ("json", "loads on each line", 0.12, False)],
      "Parsing speed", "GB/s", " GB/s")
