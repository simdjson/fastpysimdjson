import os

from setuptools import setup, Extension
from setuptools.command.build_ext import build_ext

# SIMDJSON_ENABLE_NAN_INF: accept NaN/Infinity (any case of nan, inf,
# and infinity). Off in the amalgamation unless the build defines it.
DEFINES = [("NDEBUG", None), ("SIMDJSON_ENABLE_NAN_INF", "1")]

ext = Extension(
    "fastsimdjson",
    # src/fastsimdjson.cpp includes vendor/simdjson.cpp (a unity build).
    sources=["src/fastsimdjson.cpp", "vendor/simdutf.cpp"],
    depends=["vendor/simdjson.cpp", "vendor/simdjson.h", "vendor/simdutf.h",
             "vendor/zmij.h", "vendor/zmij.cc"],
    include_dirs=["vendor"],
    language="c++",
    define_macros=DEFINES,
)


class BuildExt(build_ext):
    def build_extensions(self):
        msvc = self.compiler.compiler_type == "msvc"
        args = ["/std:c++17", "/O2", "/EHsc", "/utf-8"] if msvc else ["-std=c++17", "-O3"]
        # zmij (float formatting) is compiled as C++14: some compilers reject
        # its constant tables in C++17 mode.
        zmij_args = ["/std:c++14", "/O2", "/EHsc"] if msvc else ["-std=c++14", "-O3"]
        zmij = self.compiler.compile([os.path.join("vendor", "zmij.cc")],
                                     output_dir=self.build_temp, include_dirs=["vendor"],
                                     macros=[("NDEBUG", None)], extra_postargs=zmij_args)
        for e in self.extensions:
            e.extra_compile_args = args
            e.extra_objects = list(zmij)
        super().build_extensions()


setup(ext_modules=[ext], cmdclass={"build_ext": BuildExt})
