# Copyright (c) 2001, 2002, 2003, 2004, 2005, 2006, 2007, 2008, 2009, 2010,
# 2011, 2012, 2013, 2014, 2015, 2016, 2017, 2018, 2019, 2020, 2021, 2022, 2023
# Python Software Foundation; All Rights Reserved

# This file is part of python-zlib-ng which is distributed under the
# PYTHON SOFTWARE FOUNDATION LICENSE VERSION 2.

import functools
import os
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

from setuptools import Extension, find_packages, setup
from setuptools.command.build_ext import build_ext

ZLIB_NG_SOURCE = os.path.join("src", "zlib_ng", "zlib-ng")

SYSTEM_IS_UNIX = (sys.platform.startswith("linux") or
                  sys.platform.startswith("darwin"))
SYSTEM_IS_WINDOWS = sys.platform.startswith("win")

# Since pip builds in a temp directory by default, setting a fixed file in
# /tmp works during the entire session.
DEFAULT_CACHE_FILE = Path(tempfile.gettempdir()
                          ).absolute() / ".zlib_ng_build_cache"
BUILD_CACHE = os.environ.get("PYTHON_ZLIB_NG_BUILD_CACHE")
BUILD_CACHE_FILE = Path(os.environ.get("PYTHON_ZLIB_NG_BUILD_CACHE_FILE",
                                       DEFAULT_CACHE_FILE))

EXTENSIONS = [
        Extension("zlib_ng.zlib_ng", ["src/zlib_ng/zlib_ngmodule.c"]),
    ]


class BuildZlibNGExt(build_ext):
    def build_extension(self, ext):
        # Add option to link dynamically for packaging systems such as conda.
        # Always link dynamically on readthedocs to simplify install.
        if (os.getenv("PYTHON_ZLIB_NG_LINK_DYNAMIC") is not None or
                os.environ.get("READTHEDOCS") is not None):
            # Check for zlib_ng include directories. This is useful when
            # installing in a conda environment.
            possible_prefixes = [sys.exec_prefix, sys.base_exec_prefix]
            for prefix in possible_prefixes:
                if Path(prefix, "include", "zlib-ng.h").exists():
                    ext.include_dirs = [os.path.join(prefix, "include")]
                    ext.library_dirs = [os.path.join(prefix, "lib")]
                    break   # Only one include directory is needed.
                # On windows include is in Library apparently
                elif Path(prefix, "Library", "include", "zlib-ng.h").exists():
                    ext.include_dirs = [os.path.join(prefix, "Library",
                                                     "include")]
                    ext.library_dirs = [os.path.join(prefix, "Library", "lib")]
                    break
            if SYSTEM_IS_UNIX:
                ext.libraries = ["z-ng"]  # libz-ng.so*
            elif SYSTEM_IS_WINDOWS:
                ext.libraries = ["zlib-ng"]  # zlib-ng*.dll
            else:
                raise NotImplementedError(
                    f"Unsupported platform: {sys.platform}")
        else:
            build_dir = build_zlib_ng()
            if SYSTEM_IS_UNIX:
                ext.extra_objects = [
                    os.path.join(build_dir, "libz-ng.a")]
            elif SYSTEM_IS_WINDOWS:
                ext.extra_objects = [
                    os.path.join(build_dir, "Release", "zlibstatic-ng.lib")]
            else:
                raise NotImplementedError(
                    f"Unsupported platform: {sys.platform}")
            ext.include_dirs = [build_dir]
            # -fPIC needed for proper static linking
            # ext.extra_compile_args = ["-fPIC"]
            pass
        super().build_extension(ext)


# Use a cache to prevent zlib-ng from being build twice.
@functools.lru_cache(maxsize=None)
def build_zlib_ng():
    # Check for cache
    if BUILD_CACHE:
        if BUILD_CACHE_FILE.exists():
            cache_path = Path(BUILD_CACHE_FILE.read_text())
            if (cache_path / "zlib-ng.h").exists():
                return str(cache_path)

    # Creating temporary directories
    build_dir = tempfile.mktemp()
    shutil.copytree(ZLIB_NG_SOURCE, build_dir)

    if hasattr(os, "sched_getaffinity"):
        cpu_count = len(os.sched_getaffinity(0))
    else:  # sched_getaffinity not available on all platforms
        cpu_count = os.cpu_count() or 1  # os.cpu_count() can return None
    # Build environment is a copy of OS environment to allow user to influence
    # it.
    build_env = os.environ.copy()
    build_env["CFLAGS"] = build_env.get("CFLAGS", "") + " -fPIC"
    # Add -fPIC flag to allow static compilation
    run_args = dict(cwd=build_dir, env=build_env)
    if sys.platform == "darwin":  # Cmake does not work properly
        subprocess.run([os.path.join(build_dir, "configure")], **run_args)
        subprocess.run(["gmake", "libz-ng.a"], **run_args)
    else:
        subprocess.run(["cmake", build_dir], **run_args)
        # Do not create test suite and do not perform tests to shorten build times.
        # There is no need when stable releases of zlib-ng are used.
        subprocess.run(["cmake", "--build", build_dir, "--config", "Release",
                        "--target", "zlibstatic",
                        "-j", str(cpu_count)], **run_args)
    if BUILD_CACHE:
        BUILD_CACHE_FILE.write_text(build_dir)
    return build_dir


setup(
    name="zlib-ng",
    version="0.1.0",
    description="Drop-in replacement for zlib and gzip modules using zlib-ng",
    author="Leiden University Medical Center",
    author_email="r.h.p.vorderman@lumc.nl",  # A placeholder for now
    long_description=Path("README.rst").read_text(),
    long_description_content_type="text/x-rst",
    cmdclass={"build_ext": BuildZlibNGExt},
    license="PSF-2.0",
    keywords="zlib-ng zlib compression deflate gzip",
    zip_safe=False,
    packages=find_packages('src'),
    package_dir={'': 'src'},
    package_data={'zlib_ng': [
        '*.pyi', 'py.typed',
        # Include zlib-ng LICENSE and other relevant files with the binary distribution.
        'zlib-ng/LICENSE.md', 'zlib-ng/README.md']},
    url="https://github.com/pycompression/python-zlib-ng",
    classifiers=[
        "Programming Language :: Python :: 3 :: Only",
        "Programming Language :: Python :: 3",
        "Programming Language :: Python :: 3.7",
        "Programming Language :: Python :: 3.8",
        "Programming Language :: Python :: 3.9",
        "Programming Language :: Python :: 3.10",
        "Programming Language :: Python :: 3.11",
        "Programming Language :: Python :: Implementation :: CPython",
        "Programming Language :: Python :: Implementation :: PyPy",
        "Programming Language :: C",
        "Development Status :: 4 - Beta",
        "Topic :: System :: Archiving :: Compression",
        "License :: OSI Approved :: Python Software Foundation License",
        "Operating System :: POSIX :: Linux",
        "Operating System :: MacOS",
        "Operating System :: Microsoft :: Windows",
    ],
    python_requires=">=3.7",  # uses METH_FASTCALL
    ext_modules=EXTENSIONS
)
