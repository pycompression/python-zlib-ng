import sys

from zlib_ng import gzip_ng

with gzip_ng.open(sys.argv[1], "rb") as gzip_file:
    while True:
        block = gzip_file.read(128 * 1024)
        if not block:
            break
