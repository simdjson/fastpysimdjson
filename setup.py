from setuptools import setup, Extension

ext = Extension(
    "fastsimdjson",
    sources=["src/fastsimdjson.cpp", "vendor/simdjson.cpp", "vendor/simdutf.cpp"],
    include_dirs=["vendor"],
    language="c++",
    extra_compile_args=["-std=c++17", "-O3", "-DNDEBUG"],
)

setup(name="fastsimdjson", version="0.1.0", ext_modules=[ext])
