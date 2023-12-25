.. python-zlib-ng documentation master file, created by
   sphinx-quickstart on Fri Sep 11 15:42:56 2020.
   You can adapt this file completely to your liking, but it should at least
   contain the root `toctree` directive.

==========================================
Welcome to python-zlib-ng's documentation!
==========================================

.. toctree::
   :maxdepth: 2
   :caption: Contents:

============
Introduction
============

.. include:: includes/README.rst
   :start-after: .. introduction start
   :end-before: .. introduction end

==========
Quickstart
==========

.. include:: includes/README.rst
   :start-after: .. quickstart start
   :end-before: .. quickstart end

============
Installation
============
Installation with pip
---------------------

::

    pip install zlib-ng

Installation is supported on Linux, MacOS and Windows. On most platforms
wheels are provided.
The installation will include a staticallly linked version of zlib-ng.
If a wheel is not provided for your system the
installation will build zlib-ng first in a temporary directory. Please check the
`zlib-ng homepage <https://github.com/zlib-ng/zlib-ng>`_ for the build requirements.

The latest development version of python-zlib-ng can be installed with::

    pip install git+https://github.com/pycompression/python-zlib-ng.git

This requires having the build requirements installed.
If you wish to link
dynamically against a version of libz-ng installed on your system use::

     PYTHON_ZLIB_NG_LINK_DYNAMIC=true pip install zlib-ng --no-binary zlib-ng

Installation via conda
----------------------
Python-zlib-ng can be installed via conda, for example using
the `miniconda <https://docs.conda.io/en/latest/miniconda.html>`_ installer
with a properly setup `conda-forge
<https://conda-forge.org/docs/user/introduction.html#how-can-i-install-packages-from-conda-forge>`_
channel. When used with bioinformatics tools setting up `bioconda
<http://bioconda.github.io/user/install.html#install-conda>`_
provides a clear set of installation instructions for conda.

python-zlib-ng is available on conda-forge and can be installed with::

  conda install python-zlib-ng

This will automatically install the zlib-ng library dependency as well, since
it is available on conda-forge.

==============================================
python-zlib-ng as a dependency in your project
==============================================

.. include:: includes/README.rst
   :start-after: .. dependency start
   :end-before: .. dependency end

.. _differences-with-zlib-and-gzip-modules:

======================================
Differences with zlib and gzip modules
======================================

.. include:: includes/README.rst
   :start-after: .. differences start
   :end-before: .. differences end

==================================
API Documentation: zlib_ng.zlib_ng
==================================

.. automodule:: zlib_ng.zlib_ng
   :members:

   .. autoclass:: _Compress
      :members:

   .. autoclass:: _Decompress
      :members: 

==================================
API-documentation: zlib_ng.gzip_ng
==================================

.. automodule:: zlib_ng.gzip_ng
   :members: compress, decompress, open, BadGzipFile, GzipFile, READ_BUFFER_SIZE

   .. autoclass:: GzipNGFile
      :members:
      :special-members: __init__

===========================================
API-documentation: zlib_ng.gzip_ng_threaded
===========================================

.. automodule:: zlib_ng.gzip_ng_threaded
   :members: open

===============================
python -m zlib_ng.gzip_ng usage
===============================

.. argparse::
   :module: zlib_ng.gzip_ng
   :func: _argument_parser
   :prog: python -m zlib_ng.gzip_ng


============
Contributing
============
.. include:: includes/README.rst
   :start-after: .. contributing start
   :end-before: .. contributing end

================
Acknowledgements
================
.. include:: includes/README.rst
   :start-after: .. acknowledgements start
   :end-before: .. acknowledgements end

.. include:: includes/CHANGELOG.rst
