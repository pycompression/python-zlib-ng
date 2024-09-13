# Copyright (c) 2001, 2002, 2003, 2004, 2005, 2006, 2007, 2008, 2009, 2010,
# 2011, 2012, 2013, 2014, 2015, 2016, 2017, 2018, 2019, 2020, 2021, 2022
# Python Software Foundation; All Rights Reserved

# This file is part of python-isal which is distributed under the
# PYTHON SOFTWARE FOUNDATION LICENSE VERSION 2.

import gzip
import io
import itertools
import os
import subprocess
import sys
import tempfile
from pathlib import Path

import pytest

from zlib_ng import gzip_ng_threaded, zlib_ng

TEST_FILE = str((Path(__file__).parent / "data" / "test.fastq.gz"))


def test_threaded_read():
    with gzip_ng_threaded.open(TEST_FILE, "rb") as thread_f:
        thread_data = thread_f.read()
    with gzip.open(TEST_FILE, "rb") as f:
        data = f.read()
    assert thread_data == data


@pytest.mark.parametrize(["mode", "threads"],
                         itertools.product(["wb", "wt"], [1, 3, -1]))
def test_threaded_write(mode, threads):
    with tempfile.NamedTemporaryFile("wb", delete=False) as tmp:
        # Use a small block size to simulate many writes.
        with gzip_ng_threaded.open(tmp, mode, threads=threads,
                                   block_size=8*1024) as out_file:
            gzip_open_mode = "rb" if "b" in mode else "rt"
            with gzip.open(TEST_FILE, gzip_open_mode) as in_file:
                while True:
                    block = in_file.read(128 * 1024)
                    if not block:
                        break
                    out_file.write(block)
    with gzip.open(TEST_FILE, "rt") as test_file:
        test_data = test_file.read()
    with gzip.open(tmp.name, "rt") as test_out:
        out_data = test_out.read()
    assert test_data == out_data


def test_threaded_open_no_threads():
    with tempfile.TemporaryFile("rb") as tmp:
        klass = gzip_ng_threaded.open(tmp, "rb", threads=0)
        # igzip.IGzipFile inherits gzip.Gzipfile
        assert isinstance(klass, gzip.GzipFile)


def test_threaded_open_not_a_file_or_pathlike():
    i_am_a_tuple = (1, 2, 3)
    with pytest.raises(TypeError) as error:
        gzip_ng_threaded.open(i_am_a_tuple)
    error.match("str")
    error.match("bytes")
    error.match("file")


# Test whether threaded readers and writers throw an error rather than hang
# indefinitely.

@pytest.mark.timeout(5)
def test_threaded_read_error():
    with open(TEST_FILE, "rb") as f:
        data = f.read()
    truncated_data = data[:-8]
    with gzip_ng_threaded.open(io.BytesIO(truncated_data), "rb") as tr_f:
        with pytest.raises(EOFError):
            tr_f.read()


@pytest.mark.timeout(5)
@pytest.mark.parametrize("threads", [1, 3])
def test_threaded_write_oversized_block_no_error(threads):
    # Random bytes are incompressible, and therefore are guaranteed to
    # trigger a buffer overflow when larger than block size unless handled
    # correctly.
    data = os.urandom(1024 * 63)  # not a multiple of block_size
    with tempfile.NamedTemporaryFile(mode="wb", delete=False) as tmp:
        with gzip_ng_threaded.open(
                tmp, "wb", compresslevel=3, threads=threads,
                block_size=8 * 1024
        ) as writer:
            writer.write(data)
    with gzip.open(tmp.name, "rb") as gzipped:
        decompressed = gzipped.read()
    assert data == decompressed


@pytest.mark.timeout(5)
@pytest.mark.parametrize("threads", [1, 3])
def test_threaded_write_error(threads):
    f = gzip_ng_threaded._ThreadedGzipWriter(
        io.BytesIO(), level=3,
        threads=threads, block_size=8 * 1024)
    # Bypass the write method which should not allow blocks larger than
    # block_size.
    f._start()
    f.input_queues[0].put((os.urandom(1024 * 64), b""))
    with pytest.raises(OverflowError) as error:
        f.close()
    error.match("Compressed output exceeds buffer size")


def test_close_reader():
    tmp = io.BytesIO(Path(TEST_FILE).read_bytes())
    f = gzip_ng_threaded._ThreadedGzipReader(tmp, "rb")
    f.close()
    assert f.closed
    # Make sure double closing does not raise errors
    f.close()


@pytest.mark.parametrize("threads", [1, 3])
def test_close_writer(threads):
    f = gzip_ng_threaded._ThreadedGzipWriter(
        io.BytesIO(), threads=threads)
    f.close()
    assert f.closed
    # Make sure double closing does not raise errors
    f.close()


def test_reader_not_writable():
    with gzip_ng_threaded.open(TEST_FILE, "rb") as f:
        assert not f.writable()


def test_writer_not_readable():
    with gzip_ng_threaded.open(io.BytesIO(), "wb") as f:
        assert not f.readable()


def test_writer_wrong_level():
    with tempfile.NamedTemporaryFile("wb") as tmp:
        with pytest.raises(zlib_ng.error) as error:
            gzip_ng_threaded.open(tmp.name, mode="wb", compresslevel=42)
        error.match("Bad compression level")


def test_writer_too_low_threads():
    with pytest.raises(ValueError) as error:
        gzip_ng_threaded._ThreadedGzipWriter(io.BytesIO(), threads=0)
    error.match("threads")
    error.match("at least 1")


def test_reader_read_after_close():
    with open(TEST_FILE, "rb") as test_f:
        f = gzip_ng_threaded._ThreadedGzipReader(test_f)
        f.close()
        with pytest.raises(ValueError) as error:
            f.read(1024)
        error.match("closed")


@pytest.mark.parametrize("threads", [1, 3])
def test_writer_write_after_close(threads):
    f = gzip_ng_threaded._ThreadedGzipWriter(io.BytesIO(), threads=threads)
    f.close()
    with pytest.raises(ValueError) as error:
        f.write(b"abc")
    error.match("closed")


def test_gzip_ng_threaded_append(tmp_path):
    test_file = tmp_path / "test.txt.gz"
    with gzip_ng_threaded.open(test_file, "wb") as f:
        f.write(b"AB")
    with gzip_ng_threaded.open(test_file, mode="ab") as f:
        f.write(b"CD")
    with gzip.open(test_file, "rb") as f:
        contents = f.read()
    assert contents == b"ABCD"


def test_gzip_ng_threaded_append_text_mode(tmp_path):
    test_file = tmp_path / "test.txt.gz"
    with gzip_ng_threaded.open(test_file, "wt") as f:
        f.write("AB")
    with gzip_ng_threaded.open(test_file, mode="at") as f:
        f.write("CD")
    with gzip.open(test_file, "rt") as f:
        contents = f.read()
    assert contents == "ABCD"


def test_threaded_reader_does_not_close_stream():
    test_stream = io.BytesIO()
    test_stream.write(gzip.compress(b"thisisatest"))
    test_stream.seek(0)
    with gzip_ng_threaded.open(test_stream, "rb") as f:
        text = f.read()
    assert not test_stream.closed
    assert text == b"thisisatest"


def test_threaded_writer_does_not_close_stream():
    test_stream = io.BytesIO()
    with gzip_ng_threaded.open(test_stream, "wb") as f:
        f.write(b"thisisatest")
    assert not test_stream.closed
    test_stream.seek(0)
    assert gzip.decompress(test_stream.read()) == b"thisisatest"


@pytest.mark.timeout(5)
@pytest.mark.parametrize(
    ["mode", "threads"], itertools.product(["rb", "wb"], [1, 2]))
def test_threaded_program_can_exit_on_error(tmp_path, mode, threads):
    program = tmp_path / "no_context_manager.py"
    test_file = tmp_path / "output.gz"
    # Write 40 mb input data to saturate read buffer. Because of the repetitive
    # nature the resulting gzip file is very small (~40 KiB).
    test_file.write_bytes(gzip.compress(b"test" * (10 * 1024 * 1024)))
    with open(program, "wt") as f:
        f.write("from zlib_ng import gzip_ng_threaded\n")
        f.write(
            f"f = gzip_ng_threaded.open('{test_file}', "
            f"mode='{mode}', threads={threads})\n"
        )
        f.write("raise Exception('Error')\n")
    subprocess.run([sys.executable, str(program)])
