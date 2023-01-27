import os
import sys

from zlib_ng import gzip_ng

with open(sys.argv[1], "rb") as in_file:
    with gzip_ng.open(os.devnull, "wb") as out_gzip:
        while True:
            block = in_file.read(128 * 1024)
            if block == b"":
                break
            out_gzip.write(block)
