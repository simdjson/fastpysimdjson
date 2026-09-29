from setuptools import setup, Extension

ext = Extension(
    "fastsimdjson",
    sources=["src/fastsimdjson.cpp", "vendor/simdjson.cpp", "vendor/simdutf.cpp"],
    include_dirs=["vendor"],
    language="c++",
    # SIMDJSON_ENABLE_NAN_INF: accept NaN/Infinity (any case of nan, inf,
    # and infinity). Off in the amalgamation unless the build defines it.
    extra_compile_args=[
        "-std=c++17",
        "-O3",
        "-DNDEBUG",
        "-DSIMDJSON_ENABLE_NAN_INF=1",
    ],
)

setup(name="fastsimdjson", version="0.1.0", ext_modules=[ext])
