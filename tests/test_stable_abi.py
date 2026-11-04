"""Ownership and output boundaries shared by full and limited API builds."""
import copy
import gc
import gzip
import io
import os
import subprocess
import sys
import sysconfig
import zlib

import pytest

from zlib_ng import zlib_ng


@pytest.mark.parametrize("size", [0, 16384, 16385, 65536, 65536 + 137])
def test_output_boundaries(size):
    data = (bytes(range(256)) * ((size + 255) // 256))[:size]
    packed = zlib.compress(data)
    assert zlib_ng.decompress(packed) == data
    decoder = zlib_ng._ZlibDecompressor()
    output = [decoder.decompress(packed, 16384)]
    while not decoder.eof:
        output.append(decoder.decompress(b"", 16384))
    assert b"".join(output) == data

    reader = zlib_ng._GzipReader(io.BytesIO(gzip.compress(data)), 4096)
    assert reader.read(16384) + reader.readall() == data


@pytest.mark.parametrize("factory", [zlib_ng.compressobj,
                                     zlib_ng.decompressobj,
                                     zlib_ng._ZlibDecompressor,
                                     lambda: zlib_ng._GzipReader(io.BytesIO()),
                                     lambda: zlib_ng._ParallelCompress(65536, -1)])
@pytest.mark.skipif(sys.implementation.name != "cpython",
                    reason="Uses CPython reference counts")
def test_heap_type_lifetime(factory):
    obj = factory()
    cls = type(obj)
    gc.collect()
    before = sys.getrefcount(cls)
    for _ in range(100):
        temporary = factory()
    del temporary
    gc.collect()
    assert sys.getrefcount(cls) == before


@pytest.mark.parametrize("factory", [zlib_ng.compressobj,
                                     zlib_ng.decompressobj,
                                     zlib_ng._ZlibDecompressor,
                                     lambda: zlib_ng._GzipReader(io.BytesIO()),
                                     lambda: zlib_ng._ParallelCompress(65536, -1)])
def test_type_is_immutable_and_not_subclassable(factory):
    cls = type(factory())
    with pytest.raises(TypeError):
        cls.unexpected_attribute = None
    with pytest.raises(TypeError):
        type("Derived", (cls,), {})


@pytest.mark.parametrize("factory", [zlib_ng.compressobj, zlib_ng.decompressobj])
def test_factory_type_cannot_be_instantiated(factory):
    with pytest.raises(TypeError):
        type(factory())()


@pytest.mark.parametrize("factory", [zlib_ng.compressobj, zlib_ng.decompressobj])
@pytest.mark.skipif(sys.implementation.name != "cpython",
                    reason="Uses CPython reference counts")
def test_factory_only_types_and_copy_lifetimes(factory):
    obj = factory()
    cls = type(obj)
    with pytest.raises(TypeError):
        cls()
    gc.collect()
    before = sys.getrefcount(cls)
    for _ in range(100):
        cloned = copy.copy(obj)
        assert type(cloned) is cls
    del cloned
    gc.collect()
    assert sys.getrefcount(cls) == before


def test_module_factory_types_after_subinterpreter_import():
    # The legacy module has process-global errors. Keep the import isolated
    # from tests of exception identity in this interpreter.
    _testcapi = pytest.importorskip("_testcapi")
    if not hasattr(_testcapi, "run_in_subinterp"):
        pytest.skip("Needs _testcapi.run_in_subinterp")
    subprocess.run([sys.executable, "-c", """
import _testcapi
from zlib_ng import zlib_ng
compress_type = type(zlib_ng.compressobj())
decompress_type = type(zlib_ng.decompressobj())
assert _testcapi.run_in_subinterp('from zlib_ng import zlib_ng; '
    'zlib_ng.decompress(zlib_ng.compress(b"test"))') == 0
assert type(zlib_ng.compressobj()) is compress_type
assert type(zlib_ng.decompressobj()) is decompress_type
"""], check=True)


@pytest.mark.parametrize("size", [-1, 2**32])
def test_invalid_parallel_buffer_size(size):
    with pytest.raises(ValueError):
        zlib_ng._ParallelCompress(size, -1)


def test_failed_reader_initialization():
    with pytest.raises(TypeError):
        zlib_ng._GzipReader(object())
    with pytest.raises(ValueError):
        zlib_ng._ZlibDecompressor(wbits=100)


@pytest.mark.skipif(not sysconfig.get_config_var("Py_GIL_DISABLED"),
                    reason="Needs a free-threaded interpreter")
def test_import_keeps_the_gil_disabled():
    env = os.environ.copy()
    env.pop("PYTHON_GIL", None)
    subprocess.run([sys.executable, "-c", """
import sys
assert not sys._is_gil_enabled()
from zlib_ng import zlib_ng
assert not sys._is_gil_enabled()
assert zlib_ng.decompress(zlib_ng.compress(b'test')) == b'test'
"""], check=True, env=env)
