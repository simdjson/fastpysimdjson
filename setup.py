from setuptools import setup, Extension
from setuptools.command.build_ext import build_ext

# SIMDJSON_ENABLE_NAN_INF: accept NaN/Infinity (any case of nan, inf,
# and infinity). Off in the amalgamation unless the build defines it.
DEFINES = [("NDEBUG", None), ("SIMDJSON_ENABLE_NAN_INF", "1")]

ext = Extension(
    "fastsimdjson",
    # src/fastsimdjson.cpp includes vendor/simdjson.cpp (a unity build).
    # vendor/simdjson_arrow.cpp (NDJSON to Arrow and back) also holds zmij, which
    # writes the floats of dumps and dumpb (declared in vendor/zmij.h).
    sources=["src/fastsimdjson.cpp", "vendor/simdutf.cpp", "vendor/simdjson_arrow.cpp"],
    depends=["vendor/simdjson.cpp", "vendor/simdjson.h", "vendor/simdutf.h",
             "vendor/zmij.h", "vendor/simdjson_arrow.h"],
    include_dirs=["vendor"],
    language="c++",
    define_macros=DEFINES,
)


class BuildExt(build_ext):
    def build_extensions(self):
        msvc = self.compiler.compiler_type == "msvc"
        args = ["/std:c++17", "/O2", "/EHsc", "/utf-8"] if msvc else ["-std=c++17", "-O3"]
        for e in self.extensions:
            e.extra_compile_args = args
        super().build_extensions()


setup(ext_modules=[ext], cmdclass={"build_ext": BuildExt})
