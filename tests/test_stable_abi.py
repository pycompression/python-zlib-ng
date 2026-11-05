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


def _uses_limited_api():
    # POSIX: zlib_ng.abi3.so. Windows names a stable ABI module zlib_ng.pyd
    # and a version-specific one zlib_ng.cp314t-win_amd64.pyd.
    suffix = os.path.basename(zlib_ng.__file__).split(".")[1]
    return suffix in ("abi3", "pyd")


@pytest.mark.skipif(sys.implementation.name != "cpython",
                    reason="Needs CPython tracemalloc allocation accounting")
def test_full_api_codec_state_uses_the_raw_python_allocator():
    if _uses_limited_api():
        pytest.skip("Limited API 3.11 builds allocate codec state with "
                    "malloc; PyMem_RawMalloc needs the 3.13 stable ABI")
    # Deflate state is about 256 KiB at the default memory level.
    compressor, peak = _traced_peak(zlib_ng.compressobj)
    assert peak > 100 * 1024


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


def test_extension_types_report_this_module():
    types = [value for value in vars(zlib_ng).values()
             if isinstance(value, type)]
    types += [type(zlib_ng.compressobj()), type(zlib_ng.decompressobj())]
    assert len(types) >= 7
    for cls in types:
        assert cls.__module__.split(".")[0] == "zlib_ng", cls


def test_gzip_reader_propagates_read_lookup_errors():
    class Unreadable:
        @property
        def read(self):
            raise MemoryError("lookup failed")

    with pytest.raises(MemoryError, match="lookup failed"):
        zlib_ng._GzipReader(Unreadable())


@pytest.mark.skipif(sys.implementation.name != "cpython",
                    reason="Needs _testcapi.set_nomemory")
def test_module_initialization_failure_reports_its_error():
    pytest.importorskip("_testcapi")
    code = """
import importlib
import sys
import _testcapi
import zlib_ng.zlib_ng
failures = successes = 0
start = 0
# A module with state is initialized again on every import.
while successes < 50:
    sys.modules.pop("zlib_ng.zlib_ng", None)
    _testcapi.set_nomemory(start, start + 1)
    try:
        importlib.import_module("zlib_ng.zlib_ng")
    except MemoryError:
        failures += 1
        successes = 0
    except SystemError as error:
        # Some CPython versions fail type creation without an exception.
        # Initialization must never leave an unreported exception behind.
        assert "without raising" in str(error), (start, error)
    else:
        successes += 1
    finally:
        _testcapi.remove_mem_hooks()
    start += 1
assert failures
module = importlib.import_module("zlib_ng.zlib_ng")
assert module.decompress(module.compress(b"test")) == b"test"
"""
    result = subprocess.run([sys.executable, "-c", code],
                            capture_output=True, text=True, timeout=60)
    assert result.returncode == 0, result.stderr


@pytest.mark.parametrize("cls", [zlib_ng._ZlibDecompressor,
                                 zlib_ng._GzipReader,
                                 zlib_ng._ParallelCompress])
def test_constructible_types_keep_their_docstrings(cls):
    assert cls.__doc__


ALLOCATION_FAILURE_OPERATIONS = {
    # name: (operation, its output decoded by the standard library)
    "decompress": ("zlib_ng.decompress(packed)", "result"),
    "compressobj": ("stream_compress()", "zlib.decompress(result)"),
    "decompressobj": ("stream_decompress()", "result"),
    "_ZlibDecompressor": ("zlib_ng._ZlibDecompressor().decompress(packed)",
                          "result"),
    "_GzipReader.read": ("zlib_ng._GzipReader(io.BytesIO(gzipped))"
                         ".read(len(data))", "result"),
    "_GzipReader.readall": ("zlib_ng._GzipReader(io.BytesIO(gzipped))"
                            ".readall()", "result"),
}


@pytest.mark.skipif(sys.implementation.name != "cpython",
                    reason="Needs _testcapi.set_nomemory")
@pytest.mark.parametrize("name", ALLOCATION_FAILURE_OPERATIONS)
def test_output_allocation_failure_cleanup(name):
    pytest.importorskip("_testcapi")
    operation, decode = ALLOCATION_FAILURE_OPERATIONS[name]
    code = f"""
import gzip
import io
import random
import zlib
import _testcapi
from zlib_ng import zlib_ng
data = random.Random(240).randbytes(256 * 1024)
packed = zlib.compress(data)
gzipped = gzip.compress(data)
def stream_compress():
    obj = zlib_ng.compressobj()
    return obj.compress(data) + obj.flush()
def stream_decompress():
    obj = zlib_ng.decompressobj()
    return obj.decompress(packed) + obj.flush()
def operation():
    return {operation}
def decode(result):
    return {decode}
failures = successes = start = 0
# Fail one allocation at a time until the operation no longer reaches the
# failing allocation.
while successes < 20:
    _testcapi.set_nomemory(start, start + 1)
    try:
        result = operation()
    except MemoryError:
        failures += 1
        successes = 0
    except zlib_ng.error as error:
        # zlib-ng reports a failed state allocation as Z_MEM_ERROR (-4).
        assert "Error -4" in str(error), (start, error)
        failures += 1
        successes = 0
    else:
        _testcapi.remove_mem_hooks()
        assert decode(result) == data, start
        successes += 1
    finally:
        _testcapi.remove_mem_hooks()
    start += 1
assert failures
# A clean call after all failure paths still produces valid output.
assert decode(operation()) == data
"""
    result = subprocess.run([sys.executable, "-c", code],
                            capture_output=True, text=True, timeout=120)
    assert result.returncode == 0, result.stderr


@pytest.mark.parametrize("name,factory", [("_Compress", "compressobj"),
                                          ("_Decompress", "decompressobj")])
def test_factory_retains_type_after_module_attribute_is_deleted(name, factory):
    # The module state owns the factory types; no test-side strong reference
    # may keep the type alive.
    code = f"""
import gc
import weakref
from zlib_ng import zlib_ng
reference = weakref.ref(getattr(zlib_ng, {name!r}))
delattr(zlib_ng, {name!r})
gc.collect()
obj = getattr(zlib_ng, {factory!r})()
assert type(obj) is reference()
"""
    result = subprocess.run([sys.executable, "-c", code],
                            capture_output=True, text=True, timeout=30)
    assert result.returncode == 0, result.stderr


@pytest.mark.parametrize("size", [1, 65536, 65537])
@pytest.mark.parametrize("buffer_type", [
    bytearray, lambda size: memoryview(bytearray(size))])
def test_gzip_reader_readinto(size, buffer_type):
    data = (bytes(range(256)) * (size // 256 + 1))[:size]
    reader = zlib_ng._GzipReader(io.BytesIO(gzip.compress(data)))
    buffer = buffer_type(65536)
    first = reader.readinto(buffer)
    # Exact for a full buffer, short at the end of the stream.
    assert first == min(size, 65536)
    output = [bytes(buffer[:first])]
    while True:
        written = reader.readinto(buffer)
        if written == 0:
            break
        output.append(bytes(buffer[:written]))
    assert b"".join(output) == data
