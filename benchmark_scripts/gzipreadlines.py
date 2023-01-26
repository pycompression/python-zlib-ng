import sys

from zlib_ng import gzip_ng

with gzip_ng.open(sys.argv[1], "rb") as gzip_file:
    for line in gzip_file:
        pass
