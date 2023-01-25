/* zlib_ngmodule.c -- gzip-compatible data compression */
/* See https://github.com/zlib-ng/zlib-ng */

#define PY_SSIZE_T_CLEAN

#include "Python.h"
#include "structmember.h"         // PyMemberDef
#include "zlib-ng.h"
#include "stdbool.h"
#include "stdint.h"

#if defined(ZLIBNG_VERNUM) && ZLIBNG_VERNUM < 0x02060
#error "At least zlib-ng version 2.0.6 is required"
#endif

#define ENTER_ZLIB(obj) do {                      \
    if (!PyThread_acquire_lock((obj)->lock, 0)) { \
        Py_BEGIN_ALLOW_THREADS                    \
        PyThread_acquire_lock((obj)->lock, 1);    \
        Py_END_ALLOW_THREADS                      \
    } } while (0)
#define LEAVE_ZLIB(obj) PyThread_release_lock((obj)->lock);


/* The following parameters are copied from zutil.h, version 0.95 */
#define DEFLATED   8
#if MAX_MEM_LEVEL >= 8
#  define DEF_MEM_LEVEL 8
#else
#  define DEF_MEM_LEVEL  MAX_MEM_LEVEL
#endif

/* Initial buffer size. */
#define DEF_BUF_SIZE (16*1024)
#define DEF_MAX_INITIAL_BUF_SIZE (16 * 1024 * 1024)

static PyModuleDef zlibmodule;

static PyObject *ZlibError;
static PyTypeObject *Comptype;
static PyTypeObject *Decomptype;
static PyTypeObject *ZlibDecompressorType;
typedef struct
{
    PyObject_HEAD
    zng_stream zst;
    PyObject *unused_data;
    PyObject *unconsumed_tail;
    char eof;
    bool is_initialised;
    PyObject *zdict;
    PyThread_type_lock lock;
} compobject;

static void
zlib_error(zng_stream zst, int err, const char *msg)
{
    const char *zmsg = Z_NULL;
    /* In case of a version mismatch, zst.msg won't be initialized.
       Check for this case first, before looking at zst.msg. */
    if (err == Z_VERSION_ERROR)
        zmsg = "library version mismatch";
    if (zmsg == Z_NULL)
        zmsg = zst.msg;
    if (zmsg == Z_NULL) {
        switch (err) {
        case Z_BUF_ERROR:
            zmsg = "incomplete or truncated stream";
            break;
        case Z_STREAM_ERROR:
            zmsg = "inconsistent stream state";
            break;
        case Z_DATA_ERROR:
            zmsg = "invalid input data";
            break;
        }
    }
    if (zmsg == Z_NULL)
        PyErr_Format(ZlibError, "Error %d %s", err, msg);
    else
        PyErr_Format(ZlibError, "Error %d %s: %.200s", err, msg, zmsg);
}

static compobject *
newcompobject(PyTypeObject *type)
{
    compobject *self;
    self = PyObject_New(compobject, type);
    if (self == NULL)
        return NULL;
    self->eof = 0;
    self->is_initialised = 0;
    self->zdict = NULL;
    self->unused_data = PyBytes_FromStringAndSize("", 0);
    if (self->unused_data == NULL) {
        Py_DECREF(self);
        return NULL;
    }
    self->unconsumed_tail = PyBytes_FromStringAndSize("", 0);
    if (self->unconsumed_tail == NULL) {
        Py_DECREF(self);
        return NULL;
    }
    self->lock = PyThread_allocate_lock();
    if (self->lock == NULL) {
        Py_DECREF(self);
        PyErr_SetString(PyExc_MemoryError, "Unable to allocate lock");
        return NULL;
    }
    return self;
}

static void*
PyZlib_Malloc(voidpf ctx, uInt items, uInt size)
{
    if (size != 0 && items > (size_t)PY_SSIZE_T_MAX / size)
        return NULL;
    /* PyMem_Malloc() cannot be used: the GIL is not held when
       inflate() and deflate() are called */
    return PyMem_RawMalloc((size_t)items * (size_t)size);
}

static void
PyZlib_Free(voidpf ctx, void *ptr)
{
    PyMem_RawFree(ptr);
}

static void
arrange_input_buffer(zng_stream *zst, Py_ssize_t *remains)
{
    zst->avail_in = (uint32_t)Py_MIN((size_t)*remains, UINT32_MAX);
    *remains -= zst->avail_in;
}

static Py_ssize_t
arrange_output_buffer_with_maximum(zng_stream *zst, PyObject **buffer,
                                   Py_ssize_t length,
                                   Py_ssize_t max_length)
{
    Py_ssize_t occupied;

    if (*buffer == NULL) {
        if (!(*buffer = PyBytes_FromStringAndSize(NULL, length)))
            return -1;
        occupied = 0;
    }
    else {
        occupied = zst->next_out - (uint8_t *)PyBytes_AS_STRING(*buffer);

        if (length == occupied) {
            Py_ssize_t new_length;
            assert(length <= max_length);
            /* can not scale the buffer over max_length */
            if (length == max_length)
                return -2;
            if (length <= (max_length >> 1))
                new_length = length << 1;
            else
                new_length = max_length;
            if (_PyBytes_Resize(buffer, new_length) < 0)
                return -1;
            length = new_length;
        }
    }

    zst->avail_out = (uint32_t)Py_MIN((size_t)(length - occupied), UINT32_MAX);
    zst->next_out = (uint8_t *)PyBytes_AS_STRING(*buffer) + occupied;

    return length;
}

static Py_ssize_t
arrange_output_buffer(zng_stream *zst, PyObject **buffer, Py_ssize_t length)
{
    Py_ssize_t ret;

    ret = arrange_output_buffer_with_maximum(zst, buffer, length,
                                             PY_SSIZE_T_MAX);
    if (ret == -2)
        PyErr_NoMemory();

    return ret;
}

static PyObject *
zlib_compress_impl(PyObject *module, Py_buffer *data, int level, int wbits)
{
    PyObject *return_value = NULL;
    Py_ssize_t obuflen = DEF_BUF_SIZE;
    int32_t flush;
    zng_stream zst;

    uint8_t *ibuf = data->buf;
    Py_ssize_t ibuflen = data->len;

    zst.opaque = NULL;
    zst.zalloc = PyZlib_Malloc;
    zst.zfree = PyZlib_Free;
    zst.next_in = ibuf;
    int err = zng_deflateInit2(&zst, level, DEFLATED, wbits, DEF_MEM_LEVEL,
                               Z_DEFAULT_STRATEGY);

    switch (err) {
    case Z_OK:
        break;
    case Z_MEM_ERROR:
        PyErr_SetString(PyExc_MemoryError,
                        "Out of memory while compressing data");
        goto error;
    case Z_STREAM_ERROR:
        PyErr_SetString(ZlibError, "Bad compression level");
        goto error;
    default:
        zng_deflateEnd(&zst);
        zlib_error(zst, err, "while compressing data");
        goto error;
    }

    do {
        arrange_input_buffer(&zst, &ibuflen);
        flush = ibuflen == 0 ? Z_FINISH : Z_NO_FLUSH;

        do {
            obuflen = arrange_output_buffer(&zst, &return_value, obuflen);
            if (obuflen < 0) {
                zng_deflateEnd(&zst);
                goto error;
            }

            Py_BEGIN_ALLOW_THREADS
            err = zng_deflate(&zst, flush);
            Py_END_ALLOW_THREADS

            if (err == Z_STREAM_ERROR) {
                zng_deflateEnd(&zst);
                zlib_error(zst, err, "while compressing data");
                goto error;
            }

        } while (zst.avail_out == 0);
        assert(zst.avail_in == 0);

    } while (flush != Z_FINISH);
    assert(err == Z_STREAM_END);

    err = zng_deflateEnd(&zst);
    if (err == Z_OK) {
        if (_PyBytes_Resize(&return_value, zst.next_out -
                            (uint8_t *)PyBytes_AS_STRING(return_value)) < 0) {
            goto error;
        }
        return return_value;
    }
    else
        zlib_error(zst, err, "while finishing compression");
 error:
    Py_XDECREF(return_value);
    return NULL;
}

static PyObject *
zlib_decompress_impl(PyObject *module, Py_buffer *data, int wbits,
                     Py_ssize_t bufsize)
{
    PyObject *return_value = NULL;
    uint8_t *ibuf;
    Py_ssize_t ibuflen;
    int err, flush;
    zng_stream zst;

    if (bufsize < 0) {
        PyErr_SetString(PyExc_ValueError, "bufsize must be non-negative");
        return NULL;
    } else if (bufsize == 0) {
        bufsize = 1;
    }


    ibuf = data->buf;
    ibuflen = data->len;

    zst.opaque = NULL;
    zst.zalloc = PyZlib_Malloc;
    zst.zfree = PyZlib_Free;
    zst.avail_in = 0;
    zst.next_in = ibuf;
    err = zng_inflateInit2(&zst, wbits);

    switch (err) {
    case Z_OK:
        break;
    case Z_MEM_ERROR:
        PyErr_SetString(PyExc_MemoryError,
                        "Out of memory while decompressing data");
        goto error;
    default:
        zng_inflateEnd(&zst);
        zlib_error(zst, err, "while preparing to decompress data");
        goto error;
    }

    do {
        arrange_input_buffer(&zst, &ibuflen);
        flush = ibuflen == 0 ? Z_FINISH : Z_NO_FLUSH;

        do {
            bufsize = arrange_output_buffer(&zst, &return_value, bufsize);
            if (bufsize < 0) {
                zng_inflateEnd(&zst);
                goto error;
            }

            Py_BEGIN_ALLOW_THREADS
            err = zng_inflate(&zst, flush);
            Py_END_ALLOW_THREADS

            switch (err) {
            case Z_OK:            /* fall through */
            case Z_BUF_ERROR:     /* fall through */
            case Z_STREAM_END:
                break;
            case Z_MEM_ERROR:
                zng_inflateEnd(&zst);
                PyErr_SetString(PyExc_MemoryError,
                                "Out of memory while decompressing data");
                goto error;
            default:
                zng_inflateEnd(&zst);
                zlib_error(zst, err, "while decompressing data");
                goto error;
            }

        } while (zst.avail_out == 0);

    } while (err != Z_STREAM_END && ibuflen != 0);


    if (err != Z_STREAM_END) {
        zng_inflateEnd(&zst);
        zlib_error(zst, err, "while decompressing data");
        goto error;
    }

    err = zng_inflateEnd(&zst);
    if (err != Z_OK) {
        zlib_error(zst, err, "while finishing decompression");
        goto error;
    }

    if (_PyBytes_Resize(&return_value, zst.next_out -
                        (uint8_t *)PyBytes_AS_STRING(return_value)) < 0) {
        goto error;
    }
    return return_value;

 error:
    Py_XDECREF(return_value);
    return NULL;
}

/*[clinic input]
zlib.compressobj

    level: int(c_default="Z_DEFAULT_COMPRESSION") = Z_DEFAULT_COMPRESSION
        The compression level (an integer in the range 0-9 or -1; default is
        currently equivalent to 6).  Higher compression levels are slower,
        but produce smaller results.
    method: int(c_default="DEFLATED") = DEFLATED
        The compression algorithm.  If given, this must be DEFLATED.
    wbits: int(c_default="MAX_WBITS") = MAX_WBITS
        +9 to +15: The base-two logarithm of the window size.  Include a zlib
            container.
        -9 to -15: Generate a raw stream.
        +25 to +31: Include a gzip container.
    memLevel: int(c_default="DEF_MEM_LEVEL") = DEF_MEM_LEVEL
        Controls the amount of memory used for internal compression state.
        Valid values range from 1 to 9.  Higher values result in higher memory
        usage, faster compression, and smaller output.
    strategy: int(c_default="Z_DEFAULT_STRATEGY") = Z_DEFAULT_STRATEGY
        Used to tune the compression algorithm.  Possible values are
        Z_DEFAULT_STRATEGY, Z_FILTERED, and Z_HUFFMAN_ONLY.
    zdict: Py_buffer = None
        The predefined compression dictionary - a sequence of bytes
        containing subsequences that are likely to occur in the input data.

Return a compressor object.
[clinic start generated code]*/

static PyObject *
zlib_compressobj_impl(PyObject *module, int level, int method, int wbits,
                      int memLevel, int strategy, Py_buffer *zdict)
/*[clinic end generated code: output=8b5bed9c8fc3814d input=2fa3d026f90ab8d5]*/
{
    if (zdict->buf != NULL && (size_t)zdict->len > UINT32_MAX) {
        PyErr_SetString(PyExc_OverflowError,
                        "zdict length does not fit in an unsigned 32-bit integer");
        return NULL;
    }

    compobject *self = newcompobject(Comptype);
    if (self == NULL)
        goto error;
    self->zst.opaque = NULL;
    self->zst.zalloc = PyZlib_Malloc;
    self->zst.zfree = PyZlib_Free;
    self->zst.next_in = NULL;
    self->zst.avail_in = 0;
    int err = zng_deflateInit2(&self->zst, level, method, wbits, memLevel, strategy);
    switch (err) {
    case Z_OK:
        self->is_initialised = 1;
        if (zdict->buf == NULL) {
            goto success;
        } else {
            err = zng_deflateSetDictionary(&self->zst,
                                           zdict->buf, (uint32_t)zdict->len);
            switch (err) {
            case Z_OK:
                goto success;
            case Z_STREAM_ERROR:
                PyErr_SetString(PyExc_ValueError, "Invalid dictionary");
                goto error;
            default:
                PyErr_SetString(PyExc_ValueError, "deflateSetDictionary()");
                goto error;
            }
       }
    case Z_MEM_ERROR:
        PyErr_SetString(PyExc_MemoryError,
                        "Can't allocate memory for compression object");
        goto error;
    case Z_STREAM_ERROR:
        PyErr_SetString(PyExc_ValueError, "Invalid initialization option");
        goto error;
    default:
        zlib_error(self->zst, err, "while creating compression object");
        goto error;
    }

 error:
    Py_CLEAR(self);
 success:
    return (PyObject *)self;
}

static int
set_inflate_zdict(compobject *self)
{
    Py_buffer zdict_buf;
    if (PyObject_GetBuffer(self->zdict, &zdict_buf, PyBUF_SIMPLE) == -1) {
        return -1;
    }
    if ((size_t)zdict_buf.len > UINT32_MAX) {
        PyErr_SetString(PyExc_OverflowError,
                        "zdict length does not fit in an unsigned 32-bit integer");
        PyBuffer_Release(&zdict_buf);
        return -1;
    }
    int err;
    err = zng_inflateSetDictionary(&self->zst,
                                   zdict_buf.buf, (uint32_t)zdict_buf.len);
    PyBuffer_Release(&zdict_buf);
    if (err != Z_OK) {
        zlib_error(self->zst, err, "while setting zdict");
        return -1;
    }
    return 0;
}

/*[clinic input]
zlib.decompressobj

    wbits: int(c_default="MAX_WBITS") = MAX_WBITS
        The window buffer size and container format.
    zdict: object(c_default="NULL") = b''
        The predefined compression dictionary.  This must be the same
        dictionary as used by the compressor that produced the input data.

Return a decompressor object.
[clinic start generated code]*/

static PyObject *
zlib_decompressobj_impl(PyObject *module, int wbits, PyObject *zdict)
/*[clinic end generated code: output=3069b99994f36906 input=d3832b8511fc977b]*/
{
    if (zdict != NULL && !PyObject_CheckBuffer(zdict)) {
        PyErr_SetString(PyExc_TypeError,
                        "zdict argument must support the buffer protocol");
        return NULL;
    }

    compobject *self = newcompobject(Decomptype);
    if (self == NULL)
        return NULL;
    self->zst.opaque = NULL;
    self->zst.zalloc = PyZlib_Malloc;
    self->zst.zfree = PyZlib_Free;
    self->zst.next_in = NULL;
    self->zst.avail_in = 0;
    if (zdict != NULL) {
        Py_INCREF(zdict);
        self->zdict = zdict;
    }
    int err = zng_inflateInit2(&self->zst, wbits);
    switch (err) {
    case Z_OK:
        self->is_initialised = 1;
        if (self->zdict != NULL && wbits < 0) {
            if (set_inflate_zdict(self) < 0) {
                Py_DECREF(self);
                return NULL;
            }
        }
        return (PyObject *)self;
    case Z_STREAM_ERROR:
        Py_DECREF(self);
        PyErr_SetString(PyExc_ValueError, "Invalid initialization option");
        return NULL;
    case Z_MEM_ERROR:
        Py_DECREF(self);
        PyErr_SetString(PyExc_MemoryError,
                        "Can't allocate memory for decompression object");
        return NULL;
    default:
        zlib_error(self->zst, err, "while creating decompression object");
        Py_DECREF(self);
        return NULL;
    }
}

static void
Dealloc(compobject *self)
{
    PyObject *type = (PyObject *)Py_TYPE(self);
    PyThread_free_lock(self->lock);
    Py_XDECREF(self->unused_data);
    Py_XDECREF(self->unconsumed_tail);
    Py_XDECREF(self->zdict);
    PyObject_Free(self);
    Py_DECREF(type);
}

static void
Comp_dealloc(compobject *self)
{
    if (self->is_initialised)
        zng_deflateEnd(&self->zst);
    Dealloc(self);
}

static void
Decomp_dealloc(compobject *self)
{
    if (self->is_initialised)
        zng_inflateEnd(&self->zst);
    Dealloc(self);
}

/*[clinic input]
zlib.Compress.compress

    cls: defining_class
    data: Py_buffer
        Binary data to be compressed.
    /

Returns a bytes object containing compressed data.

After calling this function, some of the input data may still
be stored in internal buffers for later processing.
Call the flush() method to clear these buffers.
[clinic start generated code]*/

static PyObject *
zlib_Compress_compress_impl(compobject *self, Py_buffer *data)
/*[clinic end generated code: output=6731b3f0ff357ca6 input=04d00f65ab01d260]*/
{
    PyObject *return_value = NULL;
    int err;
    Py_ssize_t obuflen = DEF_BUF_SIZE;

    ENTER_ZLIB(self);

    self->zst.next_in = data->buf;
    Py_ssize_t ibuflen = data->len;

    do {
        arrange_input_buffer(&self->zst, &ibuflen);

        do {
            obuflen = arrange_output_buffer(&self->zst, &return_value, obuflen);
            if (obuflen < 0) {
                goto error;
            }

            Py_BEGIN_ALLOW_THREADS
            err = zng_deflate(&self->zst, Z_NO_FLUSH);
            Py_END_ALLOW_THREADS

            if (err == Z_STREAM_ERROR) {
                zlib_error(self->zst, err, "while compressing data");
                goto error;
            }

        } while (self->zst.avail_out == 0);
        assert(self->zst.avail_in == 0);

    } while (ibuflen != 0);

    if (_PyBytes_Resize(&return_value, self->zst.next_out -
                        (uint8_t *)PyBytes_AS_STRING(return_value)) == 0) {
        goto success;
    }

 error:
    Py_CLEAR(return_value);
 success:
    LEAVE_ZLIB(self);
    return return_value;
}

/* Helper for objdecompress() and flush(). Saves any unconsumed input data in
   self->unused_data or self->unconsumed_tail, as appropriate. */
static int
save_unconsumed_input(compobject *self, Py_buffer *data, int err)
{
    if (err == Z_STREAM_END) {
        /* The end of the compressed data has been reached. Store the leftover
           input data in self->unused_data. */
        if (self->zst.avail_in > 0) {
            Py_ssize_t old_size = PyBytes_GET_SIZE(self->unused_data);
            Py_ssize_t new_size, left_size;
            PyObject *new_data;
            left_size = (uint8_t *)data->buf + data->len - self->zst.next_in;
            if (left_size > (PY_SSIZE_T_MAX - old_size)) {
                PyErr_NoMemory();
                return -1;
            }
            new_size = old_size + left_size;
            new_data = PyBytes_FromStringAndSize(NULL, new_size);
            if (new_data == NULL)
                return -1;
            memcpy(PyBytes_AS_STRING(new_data),
                      PyBytes_AS_STRING(self->unused_data), old_size);
            memcpy(PyBytes_AS_STRING(new_data) + old_size,
                      self->zst.next_in, left_size);
            Py_SETREF(self->unused_data, new_data);
            self->zst.avail_in = 0;
        }
    }

    if (self->zst.avail_in > 0 || PyBytes_GET_SIZE(self->unconsumed_tail)) {
        /* This code handles two distinct cases:
           1. Output limit was reached. Save leftover input in unconsumed_tail.
           2. All input data was consumed. Clear unconsumed_tail. */
        Py_ssize_t left_size = (uint8_t *)data->buf + data->len - self->zst.next_in;
        PyObject *new_data = PyBytes_FromStringAndSize(
                (char *)self->zst.next_in, left_size);
        if (new_data == NULL)
            return -1;
        Py_SETREF(self->unconsumed_tail, new_data);
    }

    return 0;
}

/*[clinic input]
zlib.Decompress.decompress

    cls: defining_class
    data: Py_buffer
        The binary data to decompress.
    /
    max_length: Py_ssize_t = 0
        The maximum allowable length of the decompressed data.
        Unconsumed input data will be stored in
        the unconsumed_tail attribute.

Return a bytes object containing the decompressed version of the data.

After calling this function, some of the input data may still be stored in
internal buffers for later processing.
Call the flush() method to clear these buffers.
[clinic start generated code]*/

static PyObject *
zlib_Decompress_decompress_impl(compobject *self, PyTypeObject *cls,
                                Py_buffer *data, Py_ssize_t max_length)
/*[clinic end generated code: output=b024a93c2c922d57 input=bfb37b3864cfb606]*/
{
    int err = Z_OK;
    Py_ssize_t ibuflen;
    Py_ssize_t obuflen = DEF_BUF_SIZE;
    PyObject *return_value = NULL;
    Py_ssize_t hard_limit;

    PyObject *module = PyType_GetModule(cls);
    if (module == NULL)
        return NULL;

    if (max_length < 0) {
        PyErr_SetString(PyExc_ValueError, "max_length must be non-negative");
        return NULL;
    } else if (max_length == 0)
        hard_limit = PY_SSIZE_T_MAX;
    else
        hard_limit = max_length;

    /* limit amount of data allocated to max_length */
    if (max_length && obuflen > max_length) {
        obuflen = max_length;
    }

    ENTER_ZLIB(self);

    self->zst.next_in = data->buf;
    ibuflen = data->len;

    do {
        arrange_input_buffer(&self->zst, &ibuflen);

        do {
            obuflen = arrange_output_buffer_with_maximum(&self->zst, &return_value,
                                                         obuflen, hard_limit);
            if (obuflen == -2) {
                if (max_length > 0) {
                    goto save;
                }
                PyErr_NoMemory();
            }
            if (obuflen < 0) {
                goto abort;
            }

            Py_BEGIN_ALLOW_THREADS
            err = zng_inflate(&self->zst, Z_SYNC_FLUSH);
            Py_END_ALLOW_THREADS

            switch (err) {
            case Z_OK:            /* fall through */
            case Z_BUF_ERROR:     /* fall through */
            case Z_STREAM_END:
                break;
            default:
                if (err == Z_NEED_DICT && self->zdict != NULL) {
                    if (set_inflate_zdict(self) < 0) {
                        goto abort;
                    }
                    else
                        break;
                }
                goto save;
            }

        } while (self->zst.avail_out == 0 || err == Z_NEED_DICT);

    } while (err != Z_STREAM_END && ibuflen != 0);

 save:
    if (save_unconsumed_input(self, data, err) < 0)
        goto abort;

    if (err == Z_STREAM_END) {
        /* This is the logical place to call inflateEnd, but the old behaviour
           of only calling it on flush() is preserved. */
        self->eof = 1;
    } else if (err != Z_OK && err != Z_BUF_ERROR) {
        /* We will only get Z_BUF_ERROR if the output buffer was full
           but there wasn't more output when we tried again, so it is
           not an error condition.
        */
        zlib_error(self->zst, err, "while decompressing data");
        goto abort;
    }

    if (_PyBytes_Resize(&return_value, self->zst.next_out -
                        (uint8_t *)PyBytes_AS_STRING(return_value)) == 0) {
        goto success;
    }
 abort:
    Py_CLEAR(return_value);
 success:
    LEAVE_ZLIB(self);
    return return_value;
}

/*[clinic input]
zlib.Compress.flush

    cls: defining_class
    mode: int(c_default="Z_FINISH") = zlib.Z_FINISH
        One of the constants Z_SYNC_FLUSH, Z_FULL_FLUSH, Z_FINISH.
        If mode == Z_FINISH, the compressor object can no longer be
        used after calling the flush() method.  Otherwise, more data
        can still be compressed.
    /

Return a bytes object containing any remaining compressed data.
[clinic start generated code]*/

static PyObject *
zlib_Compress_flush_impl(compobject *self, PyTypeObject *cls, int mode)
/*[clinic end generated code: output=c7efd13efd62add2 input=286146e29442eb6c]*/
{
    int err;
    Py_ssize_t length = DEF_BUF_SIZE;
    PyObject *return_value = NULL;

    /* Flushing with Z_NO_FLUSH is a no-op, so there's no point in
       doing any work at all; just return an empty string. */
    if (mode == Z_NO_FLUSH) {
        return PyBytes_FromStringAndSize(NULL, 0);
    }

    ENTER_ZLIB(self);

    self->zst.avail_in = 0;

    do {
        length = arrange_output_buffer(&self->zst, &return_value, length);
        if (length < 0) {
            Py_CLEAR(return_value);
            goto error;
        }

        Py_BEGIN_ALLOW_THREADS
        err = zng_deflate(&self->zst, mode);
        Py_END_ALLOW_THREADS

        if (err == Z_STREAM_ERROR) {
            zlib_error(self->zst, err, "while flushing");
            Py_CLEAR(return_value);
            goto error;
        }
    } while (self->zst.avail_out == 0);
    assert(self->zst.avail_in == 0);

    /* If mode is Z_FINISH, we also have to call deflateEnd() to free
       various data structures. Note we should only get Z_STREAM_END when
       mode is Z_FINISH, but checking both for safety*/
    if (err == Z_STREAM_END && mode == Z_FINISH) {
        err = zng_deflateEnd(&self->zst);
        if (err != Z_OK) {
            zlib_error(self->zst, err, "while finishing compression");
            Py_CLEAR(return_value);
            goto error;
        }
        else
            self->is_initialised = 0;

        /* We will only get Z_BUF_ERROR if the output buffer was full
           but there wasn't more output when we tried again, so it is
           not an error condition.
        */
    } else if (err != Z_OK && err != Z_BUF_ERROR) {
        zlib_error(self->zst, err, "while flushing");
        Py_CLEAR(return_value);
        goto error;
    }

    if (_PyBytes_Resize(&return_value, self->zst.next_out -
                        (uint8_t *)PyBytes_AS_STRING(return_value)) < 0)
        Py_CLEAR(return_value);

 error:
    LEAVE_ZLIB(self);
    return return_value;
}

PyDoc_STRVAR(zlib_Compress_copy__doc__,
"copy($self, /)\n"
"--\n"
"\n"
"Return a copy of the compression object.");

#define ZLIB_COMPRESS_COPY_METHODDEF    \
    {"copy", (PyCFunction)zlib_Compress_copy, METH_NOARGS, zlib_Compress_copy__doc__}

static PyObject *
zlib_Compress_copy(compobject *self, PyObject *Py_UNUSED(ignored))
{
    compobject *return_value = newcompobject(Comptype);
    if (!return_value) return NULL;

    /* Copy the zstream state
     * We use ENTER_ZLIB / LEAVE_ZLIB to make this thread-safe
     */
    ENTER_ZLIB(self);
    int err = zng_deflateCopy(&return_value->zst, &self->zst);
    switch (err) {
    case Z_OK:
        break;
    case Z_STREAM_ERROR:
        PyErr_SetString(PyExc_ValueError, "Inconsistent stream state");
        goto error;
    case Z_MEM_ERROR:
        PyErr_SetString(PyExc_MemoryError,
                        "Can't allocate memory for compression object");
        goto error;
    default:
        zlib_error(self->zst, err, "while copying compression object");
        goto error;
    }
    Py_XSETREF(return_value->unused_data, Py_NewRef(self->unused_data));
    Py_XSETREF(return_value->unconsumed_tail, Py_NewRef(self->unconsumed_tail));
    Py_XSETREF(return_value->zdict, Py_XNewRef(self->zdict));
    return_value->eof = self->eof;

    /* Mark it as being initialized */
    return_value->is_initialised = 1;

    LEAVE_ZLIB(self);
    return (PyObject *)return_value;

error:
    LEAVE_ZLIB(self);
    Py_XDECREF(return_value);
    return NULL;
}


PyDoc_STRVAR(zlib_Compress___copy____doc__,
"__copy__($self, /)\n"
"--\n"
"\n");

#define ZLIB_COMPRESS___COPY___METHODDEF    \
    {"__copy__", (PyCFunction)zlib_Compress___copy__, METH_NOARGS, zlib_Compress___copy____doc__}

static PyObject *
zlib_Compress___copy__(compobject *self, PyObject *Py_UNUSED(ignored))
{
    return zlib_Compress_copy(self, NULL);
}

PyDoc_STRVAR(zlib_Compress___deepcopy____doc__,
"__deepcopy__($self, memo, /)\n"
"--\n"
"\n");

#define ZLIB_COMPRESS___DEEPCOPY___METHODDEF    \
    {"__deepcopy__", (PyCFunction)zlib_Compress___deepcopy__, METH_O, zlib_Compress___deepcopy____doc__}

static PyObject *
zlib_Compress___deepcopy__(compobject *self, PyObject *memo)
{
    return zlib_Compress_copy(self, NULL);
}

PyDoc_STRVAR(zlib_Decompress_copy__doc__,
"copy($self, /)\n"
"--\n"
"\n"
"Return a copy of the decompression object.");

#define ZLIB_DECOMPRESS_COPY_METHODDEF    \
    {"copy", (PyCFunction)zlib_Decompress_copy, METH_NOARGS, zlib_Decompress_copy__doc__}
static PyObject *
zlib_Decompress_copy(compobject *self, PyObject *Py_UNUSED(ignored))
{
    compobject *return_value = newcompobject(Decomptype);
    if (!return_value) return NULL;

    /* Copy the zstream state
     * We use ENTER_ZLIB / LEAVE_ZLIB to make this thread-safe
     */
    ENTER_ZLIB(self);
    int err = zng_inflateCopy(&return_value->zst, &self->zst);
    switch (err) {
    case Z_OK:
        break;
    case Z_STREAM_ERROR:
        PyErr_SetString(PyExc_ValueError, "Inconsistent stream state");
        goto error;
    case Z_MEM_ERROR:
        PyErr_SetString(PyExc_MemoryError,
                        "Can't allocate memory for decompression object");
        goto error;
    default:
        zlib_error(self->zst, err, "while copying decompression object");
        goto error;
    }

    Py_XSETREF(return_value->unused_data, Py_NewRef(self->unused_data));
    Py_XSETREF(return_value->unconsumed_tail, Py_NewRef(self->unconsumed_tail));
    Py_XSETREF(return_value->zdict, Py_XNewRef(self->zdict));
    return_value->eof = self->eof;

    /* Mark it as being initialized */
    return_value->is_initialised = 1;

    LEAVE_ZLIB(self);
    return (PyObject *)return_value;

error:
    LEAVE_ZLIB(self);
    Py_XDECREF(return_value);
    return NULL;
}

PyDoc_STRVAR(zlib_Decompress___copy____doc__,
"__copy__($self, /)\n"
"--\n"
"\n");

#define ZLIB_DECOMPRESS___COPY___METHODDEF    \
    {"__copy__", (PyCFunction)zlib_Decompress___copy__, METH_NOARGS, zlib_Decompress___copy____doc__}

static PyObject *
zlib_Decompress___copy__(compobject *self, PyTypeObject *cls)
{
    return zlib_Decompress_copy(self, NULL);
}

PyDoc_STRVAR(zlib_Decompress___deepcopy____doc__,
"__deepcopy__($self, memo, /)\n"
"--\n"
"\n");


#define ZLIB_DECOMPRESS___DEEPCOPY___METHODDEF    \
    {"__deepcopy__", (PyCFunction)zlib_Decompress___deepcopy__, METH_O, zlib_Decompress___deepcopy____doc__}

static PyObject *
zlib_Decompress___deepcopy__(compobject *self, PyObject *memo)
{
    return zlib_Decompress_copy(self, NULL);
}


/*[clinic input]
zlib.Decompress.flush

    cls: defining_class
    length: Py_ssize_t(c_default="DEF_BUF_SIZE") = zlib.DEF_BUF_SIZE
        the initial size of the output buffer.
    /

Return a bytes object containing any remaining decompressed data.
[clinic start generated code]*/

static PyObject *
zlib_Decompress_flush_impl(compobject *self, PyTypeObject *cls,
                           Py_ssize_t length)
/*[clinic end generated code: output=4532fc280bd0f8f2 input=42f1f4b75230e2cd]*/
{
    int err, flush;
    Py_buffer data;
    PyObject *return_value = NULL;
    Py_ssize_t ibuflen;

    PyObject *module = PyType_GetModule(cls);
    if (module == NULL) {
        return NULL;
    }

    if (length <= 0) {
        PyErr_SetString(PyExc_ValueError, "length must be greater than zero");
        return NULL;
    }

    ENTER_ZLIB(self);

    if (PyObject_GetBuffer(self->unconsumed_tail, &data, PyBUF_SIMPLE) == -1) {
        LEAVE_ZLIB(self);
        return NULL;
    }

    self->zst.next_in = data.buf;
    ibuflen = data.len;


    do {
        arrange_input_buffer(&self->zst, &ibuflen);
        flush = ibuflen == 0 ? Z_FINISH : Z_NO_FLUSH;

        do {
            length = arrange_output_buffer(&self->zst, &return_value, length);
            if (length < 0) {
                goto abort;
            }

            Py_BEGIN_ALLOW_THREADS
            err = zng_inflate(&self->zst, flush);
            Py_END_ALLOW_THREADS

            switch (err) {
            case Z_OK:            /* fall through */
            case Z_BUF_ERROR:     /* fall through */
            case Z_STREAM_END:
                break;
            default:
                goto save;
            }

        } while (self->zst.avail_out == 0 || err == Z_NEED_DICT);

    } while (err != Z_STREAM_END && ibuflen != 0);

 save:
    if (save_unconsumed_input(self, &data, err) < 0) {
        goto abort;
    }

    /* If at end of stream, clean up any memory allocated by zlib. */
    if (err == Z_STREAM_END) {
        self->eof = 1;
        self->is_initialised = 0;
        err = zng_inflateEnd(&self->zst);
        if (err != Z_OK) {
            zlib_error(self->zst, err, "while finishing decompression");
            goto abort;
        }
    }

    if (_PyBytes_Resize(&return_value, self->zst.next_out -
                        (uint8_t *)PyBytes_AS_STRING(return_value)) == 0) {
        goto success;
    }
 abort:
    Py_CLEAR(return_value);
 success:
    PyBuffer_Release(&data);
    LEAVE_ZLIB(self);
    return return_value;
}


typedef struct {
    PyObject_HEAD
    zng_stream zst;
    PyObject *zdict;
    PyThread_type_lock lock;
    PyObject *unused_data;
    uint8_t *input_buffer;
    Py_ssize_t input_buffer_size;
    /* zst>avail_in is only 32 bit, so we store the true length
       separately. Conversion and looping is encapsulated in
       decompress_buf() */
    Py_ssize_t avail_in_real;
    bool is_initialised;
    char eof;           /* T_BOOL expects a char */
    char needs_input;
} ZlibDecompressor;

/*[clinic input]
class zlib.ZlibDecompressor "ZlibDecompressor *" "&ZlibDecompressorType"
[clinic start generated code]*/
/*[clinic end generated code: output=da39a3ee5e6b4b0d input=0658178ab94645df]*/

static void
ZlibDecompressor_dealloc(ZlibDecompressor *self)
{
    PyObject *type = (PyObject *)Py_TYPE(self);
    PyThread_free_lock(self->lock);
    if (self->is_initialised) {
        inflateEnd(&self->zst);
    }
    PyMem_Free(self->input_buffer);
    Py_CLEAR(self->unused_data);
    Py_CLEAR(self->zdict);
    PyObject_Free(self);
    Py_DECREF(type);
}

static int
set_inflate_zdict_ZlibDecompressor(ZlibDecompressor *self)
{
    Py_buffer zdict_buf;
    if (PyObject_GetBuffer(self->zdict, &zdict_buf, PyBUF_SIMPLE) == -1) {
        return -1;
    }
    if ((size_t)zdict_buf.len > UINT32_MAX) {
        PyErr_SetString(PyExc_OverflowError,
                        "zdict length does not fit in an unsigned 32-bit integer");
        PyBuffer_Release(&zdict_buf);
        return -1;
    }
    int err;
    err = zng_inflateSetDictionary(&self->zst,
                                   zdict_buf.buf, (uint32_t)zdict_buf.len);
    PyBuffer_Release(&zdict_buf);
    if (err != Z_OK) {
        zlib_error(self->zst, err, "while setting zdict");
        return -1;
    }
    return 0;
}


/* Decompress data of length self->avail_in_real in self->state.next_in. The
   output buffer is allocated dynamically and returned. If the max_length is
   of sufficiently low size, max_length is allocated immediately. At most
   max_length bytes are returned, so some of the input may not be consumed.
   self->state.next_in and self->avail_in_real are updated to reflect the
   consumed input. */
static PyObject*
decompress_buf(ZlibDecompressor *self, Py_ssize_t max_length)
{
    /* data_size is strictly positive, but because we repeatedly have to
       compare against max_length and PyBytes_GET_SIZE we declare it as
       signed */
    PyObject *return_value = NULL;
    Py_ssize_t hard_limit;
    Py_ssize_t obuflen;

    int err = Z_OK;

    /* When sys.maxsize is passed as default use DEF_BUF_SIZE as start buffer.
       In this particular case the data may not necessarily be very big, so
       it is better to grow dynamically.*/
    if ((max_length < 0) || max_length == PY_SSIZE_T_MAX) {
        hard_limit = PY_SSIZE_T_MAX;
        obuflen = DEF_BUF_SIZE;
    } else {
        /* Assume that decompressor is used in file decompression with a fixed
           block size of max_length. In that case we will reach max_length almost
           always (except at the end of the file). So it makes sense to allocate
           max_length. */
        hard_limit = max_length;
        obuflen = max_length;
        if (obuflen > DEF_MAX_INITIAL_BUF_SIZE){
            // Safeguard against memory overflow.
            obuflen = DEF_MAX_INITIAL_BUF_SIZE;
        }
    }

    do {
        arrange_input_buffer(&(self->zst), &(self->avail_in_real));

        do {
            obuflen = arrange_output_buffer_with_maximum(&self->zst,
                                                         &return_value,
                                                         obuflen,
                                                         hard_limit);
            if (obuflen == -1){
                PyErr_SetString(PyExc_MemoryError,
                                "Insufficient memory for buffer allocation");
                goto error;
            }
            else if (obuflen == -2) {
                break;
            }
            Py_BEGIN_ALLOW_THREADS
            err = zng_inflate(&self->zst, Z_SYNC_FLUSH);
            Py_END_ALLOW_THREADS
            switch (err) {
            case Z_OK:            /* fall through */
            case Z_BUF_ERROR:     /* fall through */
            case Z_STREAM_END:
                break;
            default:
                if (err == Z_NEED_DICT) {
                    goto error;
                }
                else {
                    break;
                }
            }
        } while (self->zst.avail_out == 0);
    } while(err != Z_STREAM_END && self->avail_in_real != 0);

    if (err == Z_STREAM_END) {
        self->eof = 1;
        self->is_initialised = 0;
        /* Unlike the Decompress object we call inflateEnd here as there are no
           backwards compatibility issues */
        err = zng_inflateEnd(&self->zst);
        if (err != Z_OK) {
            zlib_error(self->zst, err, "while finishing decompression");
            goto error;
        }
    } else if (err != Z_OK && err != Z_BUF_ERROR) {
        zlib_error(self->zst, err, "while decompressing data");
    }

    self->avail_in_real += self->zst.avail_in;

    if (_PyBytes_Resize(&return_value, self->zst.next_out -
                        (uint8_t *)PyBytes_AS_STRING(return_value)) != 0) {
        goto error;
    }

    goto success;
error:
    Py_CLEAR(return_value);
success:
    return return_value;
}


static PyObject *
decompress(ZlibDecompressor *self, uint8_t *data,
           size_t len, Py_ssize_t max_length)
{
    bool input_buffer_in_use;
    PyObject *result;

    /* Prepend unconsumed input if necessary */
    if (self->zst.next_in != NULL) {
        size_t avail_now, avail_total;

        /* Number of bytes we can append to input buffer */
        avail_now = (self->input_buffer + self->input_buffer_size)
            - (self->zst.next_in + self->avail_in_real);

        /* Number of bytes we can append if we move existing
           contents to beginning of buffer (overwriting
           consumed input) */
        avail_total = self->input_buffer_size - self->avail_in_real;

        if (avail_total < len) {
            size_t offset = self->zst.next_in - self->input_buffer;
            uint8_t *tmp;
            size_t new_size = self->input_buffer_size + len - avail_now;

            /* Assign to temporary variable first, so we don't
               lose address of allocated buffer if realloc fails */
            tmp = PyMem_Realloc(self->input_buffer, new_size);
            if (tmp == NULL) {
                PyErr_SetNone(PyExc_MemoryError);
                return NULL;
            }
            self->input_buffer = tmp;
            self->input_buffer_size = new_size;

            self->zst.next_in = self->input_buffer + offset;
        }
        else if (avail_now < len) {
            memmove(self->input_buffer, self->zst.next_in,
                    self->avail_in_real);
            self->zst.next_in = self->input_buffer;
        }
        memcpy((void*)(self->zst.next_in + self->avail_in_real), data, len);
        self->avail_in_real += len;
        input_buffer_in_use = 1;
    }
    else {
        self->zst.next_in = data;
        self->avail_in_real = len;
        input_buffer_in_use = 0;
    }

    result = decompress_buf(self, max_length);
    if(result == NULL) {
        self->zst.next_in = NULL;
        return NULL;
    }

    if (self->eof) {
        self->needs_input = 0;

        if (self->avail_in_real > 0) {
            PyObject *unused_data = PyBytes_FromStringAndSize(
                (char *)self->zst.next_in, self->avail_in_real);
            if (unused_data == NULL) {
                goto error;
            }
            Py_XSETREF(self->unused_data, unused_data);
        }
    }
    else if (self->avail_in_real == 0) {
        self->zst.next_in = NULL;
        self->needs_input = 1;
    }
    else {
        self->needs_input = 0;

        /* If we did not use the input buffer, we now have
           to copy the tail from the caller's buffer into the
           input buffer */
        if (!input_buffer_in_use) {

            /* Discard buffer if it's too small
               (resizing it may needlessly copy the current contents) */
            if (self->input_buffer != NULL &&
                self->input_buffer_size < self->avail_in_real) {
                PyMem_Free(self->input_buffer);
                self->input_buffer = NULL;
            }

            /* Allocate if necessary */
            if (self->input_buffer == NULL) {
                self->input_buffer = PyMem_Malloc(self->avail_in_real);
                if (self->input_buffer == NULL) {
                    PyErr_SetNone(PyExc_MemoryError);
                    goto error;
                }
                self->input_buffer_size = self->avail_in_real;
            }

            /* Copy tail */
            memcpy(self->input_buffer, self->zst.next_in, self->avail_in_real);
            self->zst.next_in = self->input_buffer;
        }
    }
    return result;

error:
    Py_XDECREF(result);
    return NULL;
}

/*[clinic input]
zlib.ZlibDecompressor.decompress

    data: Py_buffer
    max_length: Py_ssize_t=-1

Decompress *data*, returning uncompressed data as bytes.

If *max_length* is nonnegative, returns at most *max_length* bytes of
decompressed data. If this limit is reached and further output can be
produced, *self.needs_input* will be set to ``False``. In this case, the next
call to *decompress()* may provide *data* as b'' to obtain more of the output.

If all of the input data was decompressed and returned (either because this
was less than *max_length* bytes, or because *max_length* was negative),
*self.needs_input* will be set to True.

Attempting to decompress data after the end of stream is reached raises an
EOFError.  Any data found after the end of the stream is ignored and saved in
the unused_data attribute.
[clinic start generated code]*/

static PyObject *
zlib_ZlibDecompressor_decompress_impl(ZlibDecompressor *self,
                                      Py_buffer *data, Py_ssize_t max_length)
/*[clinic end generated code: output=990d32787b775f85 input=0b29d99715250b96]*/

{
    PyObject *result = NULL;

    ENTER_ZLIB(self);
    if (self->eof) {
        PyErr_SetString(PyExc_EOFError, "End of stream already reached");
    }
    else {
        result = decompress(self, data->buf, data->len, max_length);
    }
    LEAVE_ZLIB(self);
    return result;
}

PyDoc_STRVAR(ZlibDecompressor__new____doc__,
"_ZlibDecompressor(wbits=15, zdict=b\'\')\n"
"--\n"
"\n"
"Create a decompressor object for decompressing data incrementally.\n"
"\n"
"  wbits = 15\n"
"  zdict\n"
"     The predefined compression dictionary. This is a sequence of bytes\n"
"     (such as a bytes object) containing subsequences that are expected\n"
"     to occur frequently in the data that is to be compressed. Those\n"
"     subsequences that are expected to be most common should come at the\n"
"     end of the dictionary. This must be the same dictionary as used by the\n"
"     compressor that produced the input data.\n"
"\n");

static PyObject *
ZlibDecompressor__new__(PyTypeObject *cls,
                        PyObject *args,
                        PyObject *kwargs)
{
    static char *keywords[] = {"wbits", "zdict", NULL};
    static const char * const format = "|iO:_ZlibDecompressor";
    int wbits = MAX_WBITS;
    PyObject *zdict = NULL;

    if (!PyArg_ParseTupleAndKeywords(
            args, kwargs, format, keywords, &wbits, &zdict)) {
        return NULL;
    }
    ZlibDecompressor *self = PyObject_New(ZlibDecompressor, cls);
    self->eof = 0;
    self->needs_input = 1;
    self->avail_in_real = 0;
    self->input_buffer = NULL;
    self->input_buffer_size = 0;
    self->zdict = Py_XNewRef(zdict);
    self->zst.opaque = NULL;
    self->zst.zalloc = PyZlib_Malloc;
    self->zst.zfree = PyZlib_Free;
    self->zst.next_in = NULL;
    self->zst.avail_in = 0;
    self->unused_data = PyBytes_FromStringAndSize(NULL, 0);
    if (self->unused_data == NULL) {
        Py_CLEAR(self);
        return NULL;
    }
    self->lock = PyThread_allocate_lock();
    if (self->lock == NULL) {
        Py_DECREF(self);
        PyErr_SetString(PyExc_MemoryError, "Unable to allocate lock");
        return NULL;
    }
    int err = zng_inflateInit2(&(self->zst), wbits);
    switch (err) {
        case Z_OK:
        self->is_initialised = 1;
        if (self->zdict != NULL && wbits < 0) {
            if (set_inflate_zdict_ZlibDecompressor(self) < 0) {
                Py_DECREF(self);
                return NULL;
            }
        }
        return (PyObject *)self;
    case Z_STREAM_ERROR:
        Py_DECREF(self);
        PyErr_SetString(PyExc_ValueError, "Invalid initialization option");
        return NULL;
    case Z_MEM_ERROR:
        Py_DECREF(self);
        PyErr_SetString(PyExc_MemoryError,
                        "Can't allocate memory for decompression object");
        return NULL;
    default:
        zlib_error(self->zst, err, "while creating decompression object");
        Py_DECREF(self);
        return NULL;
    }
}

PyDoc_STRVAR(zlib_adler32__doc__,
"adler32($module, data, value=1, /)\n"
"--\n"
"\n"
"Compute an Adler-32 checksum of data.\n"
"\n"
"  value\n"
"    Starting value of the checksum.\n"
"\n"
"The returned checksum is an integer.");

#define ZLIB_ADLER32_METHODDEF    \
    {"adler32", (PyCFunction)(void(*)(void))zlib_adler32, METH_FASTCALL, \
     zlib_adler32__doc__}

static PyObject *
zlib_adler32(PyObject *module, PyObject *const *args, Py_ssize_t nargs)
{
    PyObject *return_value = NULL;
    Py_buffer data = {NULL, NULL};
    uint32_t value = 1;
    Py_ssize_t len = data.len ;
    uint8_t *buf = data.buf;

    if (nargs < 1 || nargs > 2) {
        PyErr_Format(
            PyExc_TypeError, 
            "adler32 takes exactly 1 or 2 arguments, got %d", 
            nargs);
        return NULL;
    }
    if (PyObject_GetBuffer(args[0], &data, PyBUF_SIMPLE) != 0) {
        return NULL;
    }
    if (nargs > 1) {
        value = (uint32_t)PyLong_AsUnsignedLongMask(args[1]);
        if (value == (uint32_t)-1 && PyErr_Occurred()) {
            PyBuffer_Release(&data);
            return NULL;
        }
    }

    while ((size_t)len > UINT32_MAX) {
        value = zng_adler32(value, buf, UINT32_MAX);
        buf += (size_t) UINT32_MAX;
        len -= (size_t) UINT32_MAX;
    }
    value = zng_adler32(value, buf, (uint32_t)len);
    return_value = PyLong_FromUnsignedLong(value & 0xffffffffU);
    PyBuffer_Release(&data);
    return return_value;
}

PyDoc_STRVAR(zlib_crc32__doc__,
"crc32($module, data, value=0, /)\n"
"--\n"
"\n"
"Compute a CRC-32 checksum of data.\n"
"\n"
"  value\n"
"    Starting value of the checksum.\n"
"\n"
"The returned checksum is an integer.");

#define ZLIB_CRC32_METHODDEF    \
    {"crc32", (PyCFunction)(void(*)(void))zlib_crc32, METH_FASTCALL, \
     zlib_crc32__doc__}

static PyObject *
zlib_crc32(PyObject *module, PyObject *const *args, Py_ssize_t nargs)
{
    PyObject *return_value = NULL;
    Py_buffer data = {NULL, NULL};
    uint32_t value = 0;

    if (nargs < 1 || nargs > 2) {
        PyErr_Format(
            PyExc_TypeError, 
            "crc32 takes exactly 1 or 2 arguments, got %d", 
            nargs);
        return NULL;
    }
    if (PyObject_GetBuffer(args[0], &data, PyBUF_SIMPLE) != 0) {
        return NULL;
    }
    if (nargs > 1) {
        value = (uint32_t)PyLong_AsUnsignedLongMask(args[1]);
        if (value == (uint32_t)-1 && PyErr_Occurred()) {
            PyBuffer_Release(&data);
            return NULL;
        }
    }

    Py_ssize_t len = data.len ;
    uint8_t *buf = data.buf;
    while ((size_t)len > UINT32_MAX) {
        value = zng_crc32(value, buf, UINT32_MAX);
        buf += (size_t) UINT32_MAX;
        len -= (size_t) UINT32_MAX;
    }
    value = zng_crc32(value, buf, (uint32_t)len);
    return_value = PyLong_FromUnsignedLong(value & 0xffffffffU);
    PyBuffer_Release(&data);
    return return_value;
}

PyDoc_STRVAR(zlib_compress__doc__,
"compress($module, data, /, level=ISAL_DEFAULT_COMPRESSION, wbits=MAX_WBITS)\n"
"--\n"
"\n"
"Returns a bytes object containing compressed data.\n"
"\n"
"  data\n"
"    Binary data to be compressed.\n"
"  level\n"
"    Compression level, in 0-3.\n"
"  wbits\n"
"    The window buffer size and container format.");

#define ZLIB_COMPRESS_METHODDEF    \
    {"compress", (PyCFunction)(void(*)(void))zlib_compress, \
     METH_VARARGS|METH_KEYWORDS, zlib_compress__doc__}

static PyObject *
zlib_compress(PyObject *module, PyObject *args, PyObject *kwargs)
{
    static char *keywords[] = {"", "level", "wbits", NULL};
    static char *format ="y*|ii:zlib.compress";
    Py_buffer data = {NULL, NULL};
    int level = Z_DEFAULT_COMPRESSION;
    int wbits = MAX_WBITS;

    if (!PyArg_ParseTupleAndKeywords(
        args, kwargs, format, keywords, &data, &level, &wbits)) {
        return NULL;
    }

    PyObject *return_value = zlib_compress_impl(module, &data, level, wbits);
    PyBuffer_Release(&data);
    return return_value;
}

PyDoc_STRVAR(zlib_decompress__doc__,
"decompress($module, data, /, wbits=MAX_WBITS, bufsize=DEF_BUF_SIZE)\n"
"--\n"
"\n"
"Returns a bytes object containing the uncompressed data.\n"
"\n"
"  data\n"
"    Compressed data.\n"
"  wbits\n"
"    The window buffer size and container format.\n"
"  bufsize\n"
"    The initial output buffer size.");

#define ZLIB_DECOMPRESS_METHODDEF    \
    {"decompress", (PyCFunction)(void(*)(void))zlib_decompress, \
     METH_VARARGS|METH_KEYWORDS, zlib_decompress__doc__}


static PyObject *
zlib_decompress(PyObject *module, PyObject *args, PyObject *kwargs)
{
    PyObject *return_value = NULL;
    static char *keywords[] = {"", "wbits", "bufsize", NULL};
    static char *format ="y*|in:zlib.decompress";
    Py_buffer data = {NULL, NULL};
    int wbits = MAX_WBITS;
    Py_ssize_t bufsize = DEF_BUF_SIZE;

    if (!PyArg_ParseTupleAndKeywords(
        args, kwargs, format, keywords, &data, &wbits, &bufsize)) {
        return NULL;
    }
    int hist_bits;
    int flag; 
   
    return_value = zlib_decompress_impl(module, &data, wbits, bufsize);
    PyBuffer_Release(&data);
    return return_value;
}


PyDoc_STRVAR(zlib_compressobj__doc__,
"compressobj($module, /, level=ISAL_DEFAULT_COMPRESSION, method=DEFLATED,\n"
"            wbits=MAX_WBITS, memLevel=DEF_MEM_LEVEL,\n"
"            strategy=Z_DEFAULT_STRATEGY, zdict=None)\n"
"--\n"
"\n"
"Return a compressor object.\n"
"\n"
"  level\n"
"    The compression level (an integer in the range 0-9 or -1; default is\n"
"    currently equivalent to 6).  Higher compression levels are slower,\n"
"    but produce smaller results.\n"
"  method\n"
"    The compression algorithm.  If given, this must be DEFLATED.\n"
"  wbits\n"
"    * +9 to +15: The base-two logarithm of the window size.  Include a zlib\n"
"      container.\n"
"    * -9 to -15: Generate a raw stream.\n"
"    * +25 to +31: Include a gzip container.\n"
"  memLevel\n"
"    Controls the amount of memory used for internal compression state.\n"
"    Valid values range from 1 to 9.  Higher values result in higher memory\n"
"    usage, faster compression, and smaller output.\n"
"  strategy\n"
"    Used to tune the compression algorithm.  Possible values are\n"
"    Z_DEFAULT_STRATEGY, Z_FILTERED, and Z_HUFFMAN_ONLY.\n"
"  zdict\n"
"    The predefined compression dictionary - a sequence of bytes\n"
"    containing subsequences that are likely to occur in the input data.");

#define ZLIB_COMPRESSOBJ_METHODDEF    \
    {"compressobj", (PyCFunction)(void(*)(void))zlib_compressobj, \
     METH_VARARGS|METH_KEYWORDS, zlib_compressobj__doc__}

static PyObject *
zlib_compressobj(PyObject *module, PyObject *args, PyObject *kwargs)
{
    PyObject *return_value = NULL;
    static char *keywords[] = {"level", "method", "wbits", "memLevel", 
                               "strategy", "zdict", NULL};
    static char *format = "|iiiiiy*:compressobj";
    int level = Z_DEFAULT_COMPRESSION;
    int method = Z_DEFLATED;
    int wbits = MAX_WBITS;
    int memLevel = DEF_MEM_LEVEL;
    int strategy = Z_DEFAULT_STRATEGY;
    Py_buffer zdict = {NULL, NULL};

    if (!PyArg_ParseTupleAndKeywords(
            args, kwargs, format, keywords,
            &level, &method, &wbits, &memLevel, &strategy, &zdict)) {
        return NULL;
    }
    return_value = zlib_compressobj_impl(module, level, method, wbits, 
                                              memLevel, strategy, &zdict);
    PyBuffer_Release(&zdict);
    return return_value;
}

PyDoc_STRVAR(zlib_decompressobj__doc__,
"decompressobj($module, /, wbits=MAX_WBITS, zdict=b\'\')\n"
"--\n"
"\n"
"Return a decompressor object.\n"
"\n"
"  wbits\n"
"    The window buffer size and container format.\n"
"  zdict\n"
"    The predefined compression dictionary.  This must be the same\n"
"    dictionary as used by the compressor that produced the input data.");

#define ZLIB_DECOMPRESSOBJ_METHODDEF    \
    {"decompressobj", (PyCFunction)(void(*)(void))zlib_decompressobj, \
     METH_VARARGS|METH_KEYWORDS, zlib_decompressobj__doc__}

static PyObject *
zlib_decompressobj(PyObject *module, PyObject *args, PyObject *kwargs)
{
    static char *keywords[] = {"wbits", "zdict", NULL};
    static char *format = "|iO:decompressobj";
    int wbits = MAX_WBITS;
    PyObject *zdict = NULL;

    if (!PyArg_ParseTupleAndKeywords(
            args, kwargs, format, keywords,
            &wbits, &zdict)) {
        return NULL;
    }
    return zlib_decompressobj_impl(module, wbits, zdict);
}

PyDoc_STRVAR(zlib_Compress_compress__doc__,
"compress($self, data, /)\n"
"--\n"
"\n"
"Returns a bytes object containing compressed data.\n"
"\n"
"  data\n"
"    Binary data to be compressed.\n"
"\n"
"After calling this function, some of the input data may still\n"
"be stored in internal buffers for later processing.\n"
"Call the flush() method to clear these buffers.");

#define ZLIB_COMPRESS_COMPRESS_METHODDEF    \
    {"compress", (PyCFunction)(void(*)(void))zlib_Compress_compress, \
     METH_O, zlib_Compress_compress__doc__}


static PyObject *
zlib_Compress_compress(compobject *self, PyObject *data)
{
    Py_buffer data_buf;
    if (PyObject_GetBuffer(data, &data_buf, PyBUF_SIMPLE) < 0) {
        return NULL;
    }
    PyObject *return_value = zlib_Compress_compress_impl(self, &data_buf);
    PyBuffer_Release(&data_buf);
    return return_value;
}

PyDoc_STRVAR(zlib_Decompress_decompress__doc__,
"decompress($self, data, /, max_length=0)\n"
"--\n"
"\n"
"Return a bytes object containing the decompressed version of the data.\n"
"\n"
"  data\n"
"    The binary data to decompress.\n"
"  max_length\n"
"    The maximum allowable length of the decompressed data.\n"
"    Unconsumed input data will be stored in\n"
"    the unconsumed_tail attribute.\n"
"\n"
"After calling this function, some of the input data may still be stored in\n"
"internal buffers for later processing.\n"
"Call the flush() method to clear these buffers.");

#define ZLIB_DECOMPRESS_DECOMPRESS_METHODDEF    \
    {"decompress", (PyCFunction)(void(*)(void))zlib_Decompress_decompress, \
     METH_VARARGS|METH_KEYWORDS, zlib_Decompress_decompress__doc__}


static PyObject *
zlib_Decompress_decompress(compobject *self, PyObject *args, PyObject *kwargs)
{
    static char *keywords[] = {"", "max_length", NULL};
    static char *format = "y*|n:decompress";
   
    Py_buffer data = {NULL, NULL};
    Py_ssize_t max_length = 0;
    if (!PyArg_ParseTupleAndKeywords(
            args, kwargs, format, keywords, &data, &max_length)) {
        return NULL;
    }
    PyObject *return_value = zlib_Decompress_decompress_impl(self, &data, 
                                                                  max_length);
    PyBuffer_Release(&data);
    return return_value;
}

PyDoc_STRVAR(zlib_Compress_flush__doc__,
"flush($self, mode=zlib.Z_FINISH, /)\n"
"--\n"
"\n"
"Return a bytes object containing any remaining compressed data.\n"
"\n"
"  mode\n"
"    One of the constants Z_SYNC_FLUSH, Z_FULL_FLUSH, Z_FINISH.\n"
"    If mode == Z_FINISH, the compressor object can no longer be\n"
"    used after calling the flush() method.  Otherwise, more data\n"
"    can still be compressed.");

#define ZLIB_COMPRESS_FLUSH_METHODDEF    \
    {"flush", (PyCFunction)(void(*)(void))zlib_Compress_flush, \
     METH_FASTCALL|METH_KEYWORDS, zlib_Compress_flush__doc__}


static PyObject *
zlib_Compress_flush(compobject *self, 
                         PyObject *const *args, 
                         Py_ssize_t nargs, 
                         PyObject *kwnames)
{
    Py_ssize_t mode; 
    if (nargs == 0) {
        mode = Z_FINISH;
    }
    else if (nargs == 1) {
        PyObject *mode_arg = args[0];
        if (PyLong_Check(mode_arg)) {
            mode = PyLong_AsSsize_t(mode_arg);
        }
        else {
            mode = PyNumber_AsSsize_t(mode_arg, PyExc_OverflowError);
        }
        if (mode == -1 && PyErr_Occurred()) {
            return NULL;
        }
    }
    else {
        PyErr_Format(
            PyExc_TypeError,
            "flush() only takes 0 or 1 positional arguments got %d", 
            nargs
        );
        return NULL;
    }
    return zlib_Compress_flush_impl(self, mode);
}
PyDoc_STRVAR(zlib_Decompress_flush__doc__,
"flush($self, length=zlib.DEF_BUF_SIZE, /)\n"
"--\n"
"\n"
"Return a bytes object containing any remaining decompressed data.\n"
"\n"
"  length\n"
"    the initial size of the output buffer.");


#define ZLIB_DECOMPRESS_FLUSH_METHODDEF    \
    {"flush", (PyCFunction)(void(*)(void))zlib_Decompress_flush, \
     METH_FASTCALL, zlib_Decompress_flush__doc__}

static PyObject *
zlib_Decompress_flush(compobject *self, PyObject *const *args, Py_ssize_t nargs)
{
    Py_ssize_t length; 
    if (nargs == 0) {
        length = DEF_BUF_SIZE;
    }
    else if (nargs == 1) {
        PyObject *length_arg = args[0];
        if (PyLong_Check(length_arg)) {
            length = PyLong_AsSsize_t(length_arg);
        }
        else {
            length = PyNumber_AsSsize_t(length_arg, PyExc_OverflowError);
        }
        if (length == -1 && PyErr_Occurred()) {
            return NULL;
        }
    }
    else {
        PyErr_Format(
            PyExc_TypeError,
            "flush() only takes 0 or 1 positional arguments got %d", 
            nargs
        );
        return NULL;
    }
    return zlib_Decompress_flush_impl(self, length);
}

static PyMethodDef comp_methods[] =
{
    ZLIB_COMPRESS_COMPRESS_METHODDEF,
    ZLIB_COMPRESS_FLUSH_METHODDEF,
    ZLIB_COMPRESS_COPY_METHODDEF,
    ZLIB_COMPRESS___COPY___METHODDEF,
    ZLIB_COMPRESS___DEEPCOPY___METHODDEF,
    {NULL, NULL}
};

static PyMethodDef Decomp_methods[] =
{
    ZLIB_DECOMPRESS_DECOMPRESS_METHODDEF,
    ZLIB_DECOMPRESS_FLUSH_METHODDEF,
    ZLIB_DECOMPRESS_COPY_METHODDEF,
    ZLIB_DECOMPRESS___COPY___METHODDEF,
    ZLIB_DECOMPRESS___DEEPCOPY___METHODDEF,
    {NULL, NULL}
};

static PyMethodDef ZlibDecompressor_methods[] = {
    ZLIB_ZLIBDECOMPRESSOR_DECOMPRESS_METHODDEF,
    {NULL}
};

#define COMP_OFF(x) offsetof(compobject, x)
static PyMemberDef Decomp_members[] = {
    {"unused_data",     T_OBJECT, COMP_OFF(unused_data), READONLY},
    {"unconsumed_tail", T_OBJECT, COMP_OFF(unconsumed_tail), READONLY},
    {"eof",             T_BOOL,   COMP_OFF(eof), READONLY},
    {NULL},
};

PyDoc_STRVAR(ZlibDecompressor_eof__doc__,
"True if the end-of-stream marker has been reached.");

PyDoc_STRVAR(ZlibDecompressor_unused_data__doc__,
"Data found after the end of the compressed stream.");

PyDoc_STRVAR(ZlibDecompressor_needs_input_doc,
"True if more input is needed before more decompressed data can be produced.");

static PyMemberDef ZlibDecompressor_members[] = {
    {"eof", T_BOOL, offsetof(ZlibDecompressor, eof),
     READONLY, ZlibDecompressor_eof__doc__},
    {"unused_data", T_OBJECT_EX, offsetof(ZlibDecompressor, unused_data),
     READONLY, ZlibDecompressor_unused_data__doc__},
    {"needs_input", T_BOOL, offsetof(ZlibDecompressor, needs_input), READONLY,
     ZlibDecompressor_needs_input_doc},
    {NULL},
};


static PyMethodDef zlib_methods[] =
{
    ZLIB_ADLER32_METHODDEF,
    ZLIB_COMPRESS_METHODDEF,
    ZLIB_COMPRESSOBJ_METHODDEF,
    ZLIB_CRC32_METHODDEF,
    ZLIB_DECOMPRESS_METHODDEF,
    ZLIB_DECOMPRESSOBJ_METHODDEF,
    {NULL, NULL}
};

static PyType_Slot Comptype_slots[] = {
    {Py_tp_dealloc, Comp_dealloc},
    {Py_tp_methods, comp_methods},
    {0, 0},
};

static PyType_Spec Comptype_spec = {
    .name = "zlib.Compress",
    .basicsize = sizeof(compobject),
    .flags = Py_TPFLAGS_DEFAULT | Py_TPFLAGS_DISALLOW_INSTANTIATION,
    .slots= Comptype_slots,
};

static PyType_Slot Decomptype_slots[] = {
    {Py_tp_dealloc, Decomp_dealloc},
    {Py_tp_methods, Decomp_methods},
    {Py_tp_members, Decomp_members},
    {0, 0},
};

static PyType_Spec Decomptype_spec = {
    .name = "zlib.Decompress",
    .basicsize = sizeof(compobject),
    .flags = Py_TPFLAGS_DEFAULT | Py_TPFLAGS_DISALLOW_INSTANTIATION,
    .slots = Decomptype_slots,
};

static PyType_Slot ZlibDecompressor_type_slots[] = {
    {Py_tp_dealloc, ZlibDecompressor_dealloc},
    {Py_tp_members, ZlibDecompressor_members},
    {Py_tp_new, ZlibDecompressor__new__},
    {Py_tp_doc, (char *)ZlibDecompressor__new____doc__},
    {Py_tp_methods, ZlibDecompressor_methods},
    {0, 0},
};

static PyType_Spec ZlibDecompressor_type_spec = {
    .name = "zlib._ZlibDecompressor",
    .basicsize = sizeof(ZlibDecompressor),
    // Calling PyType_GetModuleState() on a subclass is not safe.
    // ZlibDecompressor_type_spec does not have Py_TPFLAGS_BASETYPE flag
    // which prevents to create a subclass.
    // So calling PyType_GetModuleState() in this file is always safe.
    .flags = (Py_TPFLAGS_DEFAULT | Py_TPFLAGS_IMMUTABLETYPE),
    .slots = ZlibDecompressor_type_slots,
};
PyDoc_STRVAR(zlib_module_documentation,
"The functions in this module allow compression and decompression using the\n"
"zlib-ng library, which is a performance enhanced drop-in replacement for zlib.\n"
"\n"
"adler32(string[, start]) -- Compute an Adler-32 checksum.\n"
"compress(data[, level]) -- Compress data, with compression level 0-9 or -1.\n"
"compressobj([level[, ...]]) -- Return a compressor object.\n"
"crc32(string[, start]) -- Compute a CRC-32 checksum.\n"
"decompress(string,[wbits],[bufsize]) -- Decompresses a compressed string.\n"
"decompressobj([wbits[, zdict]]) -- Return a decompressor object.\n"
"\n"
"'wbits' is window buffer size and container format.\n"
"Compressor objects support compress() and flush() methods; decompressor\n"
"objects support decompress() and flush().");

static int
zlib_exec(PyObject *mod)
{
    Comptype = (PyTypeObject *)PyType_FromSpec(&Comptype_spec);
    if (Comptype == NULL) {
        return -1;
    }

    Decomptype = (PyTypeObject *)PyType_FromSpec(&Decomptype_spec);
    if (Decomptype == NULL) {
        return -1;
    }

    ZlibDecompressorType = (PyTypeObject *)PyType_FromSpec(
        mod, &ZlibDecompressor_type_spec, NULL);
    if (ZlibDecompressorType == NULL) {
        return -1;
    }

    ZlibError = PyErr_NewException("zlib.error", NULL, NULL);
    if (ZlibError == NULL) {
        return -1;
    }

    if (PyModule_AddObject(mod, "error", Py_NewRef(ZlibError)) < 0) {
        Py_DECREF(ZlibError);
        return -1;
    }
    if (PyModule_AddObject(mod, "_ZlibDecompressor",
                           Py_NewRef(ZlibDecompressorType)) < 0) {
        Py_DECREF(ZlibDecompressorType);
        return -1;
    }

#define ZLIB_ADD_INT_MACRO(c)                           \
    do {                                                \
        if ((PyModule_AddIntConstant(mod, #c, c)) < 0) {  \
            return -1;                                  \
        }                                               \
    } while(0)

    ZLIB_ADD_INT_MACRO(MAX_WBITS);
    ZLIB_ADD_INT_MACRO(DEFLATED);
    ZLIB_ADD_INT_MACRO(DEF_MEM_LEVEL);
    ZLIB_ADD_INT_MACRO(DEF_BUF_SIZE);
    // compression levels
    ZLIB_ADD_INT_MACRO(Z_NO_COMPRESSION);
    ZLIB_ADD_INT_MACRO(Z_BEST_SPEED);
    ZLIB_ADD_INT_MACRO(Z_BEST_COMPRESSION);
    ZLIB_ADD_INT_MACRO(Z_DEFAULT_COMPRESSION);
    // compression strategies
    ZLIB_ADD_INT_MACRO(Z_FILTERED);
    ZLIB_ADD_INT_MACRO(Z_HUFFMAN_ONLY);
#ifdef Z_RLE // 1.2.0.1
    ZLIB_ADD_INT_MACRO(Z_RLE);
#endif
#ifdef Z_FIXED // 1.2.2.2
    ZLIB_ADD_INT_MACRO(Z_FIXED);
#endif
    ZLIB_ADD_INT_MACRO(Z_DEFAULT_STRATEGY);
    // allowed flush values
    ZLIB_ADD_INT_MACRO(Z_NO_FLUSH);
    ZLIB_ADD_INT_MACRO(Z_PARTIAL_FLUSH);
    ZLIB_ADD_INT_MACRO(Z_SYNC_FLUSH);
    ZLIB_ADD_INT_MACRO(Z_FULL_FLUSH);
    ZLIB_ADD_INT_MACRO(Z_FINISH);
#ifdef Z_BLOCK // 1.2.0.5 for inflate, 1.2.3.4 for deflate
    ZLIB_ADD_INT_MACRO(Z_BLOCK);
#endif
#ifdef Z_TREES // 1.2.3.4, only for inflate
    ZLIB_ADD_INT_MACRO(Z_TREES);
#endif
    PyObject *ver = PyUnicode_FromString(ZLIBNG_VERSION);
    if (ver == NULL) {
        return -1;
    }

    if (PyModule_AddObject(mod, "ZLIBNG_VERSION", ver) < 0) {
        Py_DECREF(ver);
        return -1;
    }

    ver = PyUnicode_FromString(zlibng_version());
    if (ver == NULL) {
        return -1;
    }

    if (PyModule_AddObject(mod, "ZLIBNG_RUNTIME_VERSION", ver) < 0) {
        Py_DECREF(ver);
        return -1;
    }

    return 0;
}

static PyModuleDef_Slot zlib_slots[] = {
    {Py_mod_exec, zlib_exec},
    {0, NULL}
};

static struct PyModuleDef zlibmodule = {
    PyModuleDef_HEAD_INIT,
    .m_name = "zlib",
    .m_doc = zlib_module_documentation,
    .m_methods = zlib_methods,
    .m_slots = zlib_slots,
    .m_traverse = zlib_traverse,
    .m_clear = zlib_clear,
    .m_free = zlib_free,
};

PyMODINIT_FUNC
PyInit_zlib(void)
{
    return PyModuleDef_Init(&zlibmodule);
}
