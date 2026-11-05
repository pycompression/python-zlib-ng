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


def _traced_peak(operation):
    """Return the result of operation() and the traced memory it peaked at."""
    tracemalloc = pytest.importorskip("tracemalloc")
    gc.collect()
    tracing = tracemalloc.is_tracing()
    if not tracing:
        tracemalloc.start()
    try:
        before, _ = tracemalloc.get_traced_memory()
        tracemalloc.reset_peak()
        result = operation()
        _, peak = tracemalloc.get_traced_memory()
    finally:
        if not tracing:
            tracemalloc.stop()
    return result, peak - before


# 1 MiB + 137 stays below the 16 MiB initial-allocation cap, so the output
# limit is allocated at once and a filled result can be returned as is.
FILLED_SIZE = 1024 * 1024 + 137


@pytest.mark.skipif(sys.implementation.name != "cpython",
                    reason="Needs CPython tracemalloc allocation accounting")
def test_filled_bounded_decompressor_output_is_not_copied():
    data = (b"filled buffer" * (FILLED_SIZE // 13 + 1))[:FILLED_SIZE]
    packed = zlib.compress(data)
    decoder = zlib_ng._ZlibDecompressor()
    output, peak = _traced_peak(
        lambda: decoder.decompress(packed, max_length=len(data)))
    assert output == data
    # Codec state fits well within this margin; a second output copy does not.
    assert peak < len(data) * 3 // 2


@pytest.mark.skipif(sys.implementation.name != "cpython",
                    reason="Needs CPython tracemalloc allocation accounting")
def test_filled_gzip_reader_chunk_is_not_copied():
    data = (b"filled buffer" * (FILLED_SIZE // 13 + 1))[:FILLED_SIZE]
    # read() fills at most 10 * buffersize bytes per call.
    reader = zlib_ng._GzipReader(io.BytesIO(gzip.compress(data)),
                                 len(data) // 10 + 1)
    output, peak = _traced_peak(lambda: reader.read(len(data)))
    assert output == data
    assert peak < len(data) * 3 // 2


@pytest.mark.parametrize("call", [
    lambda: zlib_ng.crc32_combine("x", 1, 1),
    lambda: zlib_ng._ParallelCompress("x"),
    # A bad first seek argument would leave the offset unset; use whence.
    lambda: zlib_ng._GzipReader(io.BytesIO(gzip.compress(b"x"))).seek(0, "x"),
    lambda: zlib_ng._GzipReader(io.BytesIO(gzip.compress(b"x"))).read("x"),
])
def test_argument_parsing_failure_raises_type_error(call):
    with pytest.raises(TypeError):
        call()
