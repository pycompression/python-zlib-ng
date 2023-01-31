import os
import sys

from zlib_ng import gzip_ng

with open(sys.argv[1], "rb") as in_file:
    with gzip_ng.open(os.devnull, "wb") as out_gzip:
        for line in in_file:
            out_gzip.write(line)
