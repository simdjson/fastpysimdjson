from setuptools import setup, Extension
from setuptools.command.build_ext import build_ext

# SIMDJSON_ENABLE_NAN_INF: accept NaN/Infinity (any case of nan, inf,
# and infinity). Off in the amalgamation unless the build defines it.
DEFINES = [("NDEBUG", None), ("SIMDJSON_ENABLE_NAN_INF", "1")]

ext = Extension(
    "fastsimdjson",
    # src/fastsimdjson.cpp includes vendor/simdjson.cpp (a unity build).
    sources=["src/fastsimdjson.cpp", "vendor/simdutf.cpp"],
    depends=["vendor/simdjson.cpp", "vendor/simdjson.h", "vendor/simdutf.h"],
    include_dirs=["vendor"],
    language="c++",
    define_macros=DEFINES,
)


class BuildExt(build_ext):
    def build_extensions(self):
        if self.compiler.compiler_type == "msvc":
            args = ["/std:c++17", "/O2", "/EHsc", "/utf-8"]
        else:
            args = ["-std=c++17", "-O3"]
        for e in self.extensions:
            e.extra_compile_args = args
        super().build_extensions()


setup(ext_modules=[ext], cmdclass={"build_ext": BuildExt})
