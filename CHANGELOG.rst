==========
Changelog
==========

.. Newest changes should be on top.

.. This document is user facing. Please word the changes in such a way
.. that users understand how the changes affect the new version.

version 0.2.0-dev
-----------------
+ Escape GIL for adler32 and crc32 functions.

version 0.1.0
-----------------
+ Build wheels for all three major operating systems.
+ Add a fully featured gzip application in python m zlib_ng.gzip_ng.
+ Port Cpython's gzip module to use zlib-ng.
+ Port CPython's zlib module to use zlib-ng.
+ Use zlib-ng version 2.0.6 as included statically linked version.