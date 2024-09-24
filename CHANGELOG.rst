==========
Changelog
==========

.. Newest changes should be on top.

.. This document is user facing. Please word the changes in such a way
.. that users understand how the changes affect the new version.

version 0.5.0
-----------------
+ Wheels are now build for MacOS arm64 architectures.
+ Fix a bug where READ and WRITE in zlib_ng.gzip_ng were inconsistent with the
  values in gzip on Python 3.13
+ Small simplifications to the ``gzip_ng.compress`` and ``gzip_ng.decompress``
  functions, which should lead to less overhead.

version 0.4.3
-----------------
+ Fix a bug where files larger than 4GB could not be decompressed.

version 0.4.2
-----------------
+ Fix a reference counting error that happened on module initialization and
  triggered an error in the CPython debug build.
+ Fix a setup.py error that was triggered on MacOS ARM64.

version 0.4.1
-----------------
+ Fix a bug where streams that were passed to gzip_ng_threaded.open where
  closed.
+ Fix compatibility with Python 3.13

version 0.4.0
-----------------
+ Add a ``gzip_ng_threaded`` module that contains the ``gzip_ng_threaded.open``
  function. This allows using multithreaded compression as well as escaping the
  GIL.
+ The internal ``gzip_ng._GzipReader`` has been rewritten in C. As a result the
  overhead of decompressing files has significantly been reduced.
+ The ``gzip_ng._GzipReader`` in C is now used in ``gzip_ng.decompress``. The
  ``_GzipReader`` also can read from objects that support the buffer protocol.
  This has reduced overhead significantly.
+ Fix some unclosed buffer errors in the gzip_ng CLI.

version 0.3.0
-----------------
+ Source distributions on Linux now default to building with configure and
  make as it is faster and has less dependencies than CMake.
+ Python 3.12 support was added. Python 3.7 support was dropped as it is end
  of life.
+ Enabled installation on BSD
+ Update embedded zlib-ng version to 2.1.5. This comes with some speed
  improvements and changes with regards to the compression levels. Also
  several bugs were fixed. For full
  details checkout the `zlib-ng 2.1.2 release notes
  <https://github.com/zlib-ng/zlib-ng/releases/tag/2.1.2>`_ as well as
  those for the bugfix releases `2.1.3
  <https://github.com/zlib-ng/zlib-ng/releases/tag/2.1.3>`_,
  `2.1.4 <https://github.com/zlib-ng/zlib-ng/releases/tag/2.1.4>`_ and
  `2.1.5 <https://github.com/zlib-ng/zlib-ng/releases/tag/2.1.5>`_.


version 0.2.0
-----------------
+ Update embedded zlib-ng version to 2.0.7
+ Escape GIL for adler32 and crc32 functions.

version 0.1.0
-----------------
+ Build wheels for all three major operating systems.
+ Add a fully featured gzip application in python m zlib_ng.gzip_ng.
+ Port Cpython's gzip module to use zlib-ng.
+ Port CPython's zlib module to use zlib-ng.
+ Use zlib-ng version 2.0.6 as included statically linked version.
