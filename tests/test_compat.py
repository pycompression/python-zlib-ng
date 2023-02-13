# Copyright (c) 2001, 2002, 2003, 2004, 2005, 2006, 2007, 2008, 2009, 2010,
# 2011, 2012, 2013, 2014, 2015, 2016, 2017, 2018, 2019, 2020, 2021, 2022
# Python Software Foundation; All Rights Reserved

# This file is part of python-isal which is distributed under the
# PYTHON SOFTWARE FOUNDATION LICENSE VERSION 2.

# This file does not include original code from CPython. It is used to ensure
# that compression and decompression between CPython's zlib and zlib_ng
# is compatible.

import gzip
import itertools
import zlib
from pathlib import Path

import pytest

from zlib_ng import gzip_ng, zlib_ng

DATA_DIR = Path(__file__).parent / "data"
COMPRESSED_FILE = DATA_DIR / "test.fastq.gz"
with gzip.open(str(COMPRESSED_FILE), mode="rb") as file_h:
    DATA = file_h.read()

DATA_SIZES = [2**i for i in range(3, 20)]
# 100 seeds generated with random.randint(0, 2**32-1)
SEEDS_FILE = DATA_DIR / "seeds.txt"
INT_OVERFLOW = 211928379812738912738917238971289378912379823871932719823798123
# Get some negative ints and some really big ints into the mix.
SEEDS = [-INT_OVERFLOW, -3, -1, 0, 1, INT_OVERFLOW] + [
    int(seed) for seed in SEEDS_FILE.read_text().splitlines()]

# Wbits for ZLIB compression, GZIP compression, and RAW compressed streams
WBITS_RANGE = list(range(9, 16)) + list(range(25, 32)) + list(range(-15, -8))

ZLIBNG_STRATEGIES = (zlib_ng.Z_DEFAULT_STRATEGY, zlib_ng.Z_FILTERED,
                     zlib_ng.Z_HUFFMAN_ONLY, zlib_ng.Z_RLE, zlib_ng.Z_FIXED)

ZLIB_STRATEGIES = [zlib.Z_DEFAULT_STRATEGY, zlib.Z_FILTERED,
                   zlib.Z_HUFFMAN_ONLY]
if hasattr(zlib, "Z_RLE"):
    ZLIB_STRATEGIES.append(zlib.Z_RLE)
if hasattr(zlib, "Z_FIXED"):
    ZLIB_STRATEGIES.append(zlib.Z_FIXED)


def limited_zlib_tests(strategies=ZLIB_STRATEGIES):
    """
    Test all combinations of memlevel compression level and wbits, but
    only for the default strategy. Test other strategies with default settings.
    """
    DEFAULT_DATA_SIZE = 128 * 1024
    compression_levels = range(-1, 10)
    memory_levels = list(range(1, 10))
    for compresslevel in compression_levels:
        for wbits in WBITS_RANGE:
            for memlevel in memory_levels:
                yield (DEFAULT_DATA_SIZE, compresslevel, wbits, memlevel,
                       zlib.Z_DEFAULT_STRATEGY)
    for strategy in strategies:
        yield (DEFAULT_DATA_SIZE, -1, zlib.MAX_WBITS, zlib.DEF_MEM_LEVEL,
               strategy)


@pytest.mark.parametrize(["data_size", "value"],
                         itertools.product(DATA_SIZES, SEEDS))
def test_crc32(data_size, value):
    data = DATA[:data_size]
    assert zlib.crc32(data, value) == zlib_ng.crc32(data, value)


@pytest.mark.parametrize(["data_size", "value"],
                         itertools.product(DATA_SIZES, SEEDS))
def test_adler32(data_size, value):
    data = DATA[:data_size]
    assert zlib.adler32(data, value) == zlib_ng.adler32(data, value)


@pytest.mark.parametrize(["data_size", "level", "wbits"],
                         itertools.product(DATA_SIZES, range(10), WBITS_RANGE))
def test_compress(data_size, level, wbits):
    data = DATA[:data_size]
    compressed = zlib_ng.compress(data, level=level, wbits=wbits)
    decompressed = zlib.decompress(compressed, wbits)
    assert decompressed == data


@pytest.mark.parametrize(["data_size", "level"],
                         itertools.product(DATA_SIZES, range(10)))
def test_decompress_zlib(data_size, level):
    data = DATA[:data_size]
    compressed = zlib.compress(data, level=level)
    decompressed = zlib_ng.decompress(compressed)
    assert decompressed == data


@pytest.mark.parametrize(["data_size", "level", "wbits", "memLevel", "strategy"],
                         limited_zlib_tests(ZLIB_STRATEGIES))
def test_decompress_wbits(data_size, level, wbits, memLevel, strategy):
    data = DATA[:data_size]
    compressobj = zlib.compressobj(level=level, wbits=wbits, memLevel=memLevel,
                                   strategy=strategy)
    compressed = compressobj.compress(data) + compressobj.flush()
    decompressed = zlib_ng.decompress(compressed, wbits=wbits)
    assert data == decompressed


@pytest.mark.parametrize(["data_size", "level", "wbits"],
                         itertools.product([128 * 1024], range(10), WBITS_RANGE),)
def test_decompress_zlib_ng(data_size, level, wbits):
    data = DATA[:data_size]
    compressed = zlib_ng.compress(data, level=level, wbits=wbits)
    decompressed = zlib_ng.decompress(compressed, wbits=wbits)
    assert decompressed == data


@pytest.mark.parametrize(["data_size", "level", "wbits", "memLevel", "strategy"],
                         limited_zlib_tests(ZLIBNG_STRATEGIES))
def test_compress_compressobj(data_size, level, wbits, memLevel, strategy):
    data = DATA[:data_size]
    compressobj = zlib_ng.compressobj(level=level,
                                      wbits=wbits,
                                      memLevel=memLevel,
                                      strategy=strategy)
    compressed = compressobj.compress(data) + compressobj.flush()
    decompressed = zlib.decompress(compressed, wbits=wbits)
    assert data == decompressed


@pytest.mark.parametrize(["data_size", "level", "wbits", "memLevel", "strategy"],
                         limited_zlib_tests(ZLIB_STRATEGIES))
def test_decompress_decompressobj(data_size, level, wbits, memLevel, strategy):
    data = DATA[:data_size]
    compressobj = zlib.compressobj(level=level, wbits=wbits, memLevel=memLevel,
                                   strategy=strategy)
    compressed = compressobj.compress(data) + compressobj.flush()
    decompressobj = zlib_ng.decompressobj(wbits=wbits)
    decompressed = decompressobj.decompress(compressed) + decompressobj.flush()
    assert data == decompressed
    assert decompressobj.unused_data == b""
    assert decompressobj.unconsumed_tail == b""


def test_decompressobj_unconsumed_tail():
    data = DATA[:128*1024]
    compressed = zlib.compress(data)
    decompressobj = zlib_ng.decompressobj()
    output = decompressobj.decompress(compressed, 2048)
    assert len(output) == 2048


@pytest.mark.parametrize(["data_size", "level"],
                         itertools.product(DATA_SIZES, range(10)))
def test_gzip_ng_compress(data_size, level):
    data = DATA[:data_size]
    compressed = gzip_ng.compress(data, compresslevel=level)
    assert gzip.decompress(compressed) == data


@pytest.mark.parametrize(["data_size", "level"],
                         itertools.product(DATA_SIZES, range(10)))
def test_decompress_gzip(data_size, level):
    data = DATA[:data_size]
    compressed = gzip.compress(data, compresslevel=level)
    decompressed = gzip_ng.decompress(compressed)
    assert decompressed == data


@pytest.mark.parametrize(["data_size", "level"],
                         itertools.product(DATA_SIZES, range(10)))
def test_decompress_gzip_ng(data_size, level):
    data = DATA[:data_size]
    compressed = gzip_ng.compress(data, compresslevel=level)
    decompressed = gzip_ng.decompress(compressed)
    assert decompressed == data


@pytest.mark.parametrize(["unused_size", "wbits"],
                         itertools.product([26], [-15, 15, 31]))
def test_unused_data(unused_size, wbits):
    unused_data = b"abcdefghijklmnopqrstuvwxyz"[:unused_size]
    compressor = zlib.compressobj(wbits=wbits)
    data = b"A meaningful sentence starts with a capital and ends with a."
    compressed = compressor.compress(data) + compressor.flush()
    decompressor = zlib_ng.decompressobj(wbits=wbits)
    result = decompressor.decompress(compressed + unused_data)
    assert result == data
    assert decompressor.unused_data == unused_data
