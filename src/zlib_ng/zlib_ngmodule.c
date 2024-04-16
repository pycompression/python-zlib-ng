/* zlib_ngmodule.c -- gzip-compatible data compression */
/* See https://github.com/zlib-ng/zlib-ng */

#define PY_SSIZE_T_CLEAN

#include "Python.h"
#include "structmember.h"         // PyMemberDef
#include "zlib-ng.h"
#include "stdbool.h"
#include "stdint.h"

#if defined(ZLIBNG_VERNUM) && ZLIBNG_VERNUM < 0x02070
#error "At least zlib-ng version 2.0.7 is required"
#endif

/* PyPy quirks: no Py_UNREACHABLE and requires PyBUF_READ and PyBUF_WRITE set 
   in memoryviews that enter a "readinto" call. CPython requires that only 
   PyBUF_WRITE is set. 
   (Both implementations are wrong because the state of the READ bit should 
    not matter.)
*/
#ifdef PYPY_VERSION
#define Py_UNREACHABLE() Py_FatalError("Reached unreachable state")
#define MEMORYVIEW_READINTO_FLAGS (PyBUF_READ | PyBUF_WRITE)
#else
#define MEMORYVIEW_READINTO_FLAGS PyBUF_WRITE
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
static PyTypeObject Comptype;
static PyTypeObject Decomptype;
static PyTypeObject ZlibDecompressorType;
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


static PyObject *
zlib_compressobj_impl(PyObject *module, int level, int method, int wbits,
                      int memLevel, int strategy, Py_buffer *zdict)
{
    if (zdict->buf != NULL && (size_t)zdict->len > UINT32_MAX) {
        PyErr_SetString(PyExc_OverflowError,
                        "zdict length does not fit in an unsigned 32-bit integer");
        return NULL;
    }

    compobject *self = newcompobject(&Comptype);
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

static PyObject *
zlib_decompressobj_impl(PyObject *module, int wbits, PyObject *zdict)
{
    if (zdict != NULL && !PyObject_CheckBuffer(zdict)) {
        PyErr_SetString(PyExc_TypeError,
                        "zdict argument must support the buffer protocol");
        return NULL;
    }

    compobject *self = newcompobject(&Decomptype);
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
    PyThread_free_lock(self->lock);
    Py_XDECREF(self->unused_data);
    Py_XDECREF(self->unconsumed_tail);
    Py_XDECREF(self->zdict);
    PyObject_Free(self);
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

static PyObject *
zlib_Compress_compress_impl(compobject *self, Py_buffer *data)
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

static PyObject *
zlib_Decompress_decompress_impl(compobject *self,
                                Py_buffer *data, Py_ssize_t max_length)
{
    int err = Z_OK;
    Py_ssize_t ibuflen;
    Py_ssize_t obuflen = DEF_BUF_SIZE;
    PyObject *return_value = NULL;
    Py_ssize_t hard_limit;

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

static PyObject *
zlib_Compress_flush_impl(compobject *self, int mode)
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
    
    compobject *return_value = newcompobject(&Comptype);
    if (!return_value) return NULL;

    if (!self->is_initialised) {
        PyErr_SetString(PyExc_ValueError, "Cannot copy flushed objects.");
        goto error;
    }

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
    Py_INCREF(self->unused_data);
    Py_INCREF(self->unconsumed_tail);
    Py_XINCREF(self->zdict);
    Py_XSETREF(return_value->unused_data, self->unused_data);
    Py_XSETREF(return_value->unconsumed_tail, self->unconsumed_tail);
    Py_XSETREF(return_value->zdict, self->zdict);
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
    compobject *return_value = newcompobject(&Decomptype);
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

    Py_INCREF(self->unused_data);
    Py_INCREF(self->unconsumed_tail);
    Py_XINCREF(self->zdict);
    Py_XSETREF(return_value->unused_data, self->unused_data);
    Py_XSETREF(return_value->unconsumed_tail, self->unconsumed_tail);
    Py_XSETREF(return_value->zdict, self->zdict);
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


static PyObject *
zlib_Decompress_flush_impl(compobject *self,
                           Py_ssize_t length)
{
    int err, flush;
    Py_buffer data;
    PyObject *return_value = NULL;
    Py_ssize_t ibuflen;

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


static void
ZlibDecompressor_dealloc(ZlibDecompressor *self)
{
    PyThread_free_lock(self->lock);
    if (self->is_initialised) {
        zng_inflateEnd(&self->zst);
    }
    PyMem_Free(self->input_buffer);
    Py_CLEAR(self->unused_data);
    Py_CLEAR(self->zdict);
    PyObject_Free(self);
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
        goto error;
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

PyDoc_STRVAR(zlib_ZlibDecompressor_decompress__doc__,
"decompress($self, /, data, max_length=-1)\n"
"--\n"
"\n"
"Decompress *data*, returning uncompressed data as bytes.\n"
"\n"
"If *max_length* is nonnegative, returns at most *max_length* bytes of\n"
"decompressed data. If this limit is reached and further output can be\n"
"produced, *self.needs_input* will be set to ``False``. In this case, the next\n"
"call to *decompress()* may provide *data* as b\'\' to obtain more of the output.\n"
"\n"
"If all of the input data was decompressed and returned (either because this\n"
"was less than *max_length* bytes, or because *max_length* was negative),\n"
"*self.needs_input* will be set to True.\n"
"\n"
"Attempting to decompress data after the end of stream is reached raises an\n"
"EOFError.  Any data found after the end of the stream is ignored and saved in\n"
"the unused_data attribute.");

#define ZLIB_ZLIBDECOMPRESSOR_DECOMPRESS_METHODDEF    \
    {"decompress", (PyCFunction)(void(*)(void))zlib_ZlibDecompressor_decompress, \
     METH_VARARGS|METH_KEYWORDS, zlib_ZlibDecompressor_decompress__doc__}

static PyObject *
zlib_ZlibDecompressor_decompress(ZlibDecompressor *self, PyObject *args, PyObject *kwargs)
{
    static char *keywords[] = {"", "max_length", NULL};
    static char *format = "y*|n:decompress";

    PyObject *result = NULL;
    Py_buffer data = {NULL, NULL};
    Py_ssize_t max_length = -1;
    if (!PyArg_ParseTupleAndKeywords(
            args, kwargs, format, keywords, &data, &max_length)) {
        return NULL;
    }

    ENTER_ZLIB(self);
    if (self->eof) {
        PyErr_SetString(PyExc_EOFError, "End of stream already reached");
    }
    else {
        result = decompress(self, data.buf, data.len, max_length);
    }
    LEAVE_ZLIB(self);
    PyBuffer_Release(&data);
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
    Py_XINCREF(zdict);
    self->zdict = zdict;
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

    Py_ssize_t len = data.len ;
    uint8_t *buf = data.buf;

    /* Do not drop GIL for small values as it increases overhead */
    if (len > 1024 * 5) {
        Py_BEGIN_ALLOW_THREADS
        while ((size_t)len > UINT32_MAX) {
            value = zng_adler32(value, buf, UINT32_MAX);
            buf += (size_t) UINT32_MAX;
            len -= (size_t) UINT32_MAX;
        }
        value = zng_adler32(value, buf, (uint32_t)len);
        Py_END_ALLOW_THREADS
    } else {
        value = zng_adler32(value, buf, (uint32_t)len);
    }

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

    /* Do not drop GIL for small values as it increases overhead */
    if (len > 1024 * 5) {
        Py_BEGIN_ALLOW_THREADS
        while ((size_t)len > UINT32_MAX) {
            value = zng_crc32(value, buf, UINT32_MAX);
            buf += (size_t) UINT32_MAX;
            len -= (size_t) UINT32_MAX;
        }
        value = zng_crc32(value, buf, (uint32_t)len);
        Py_END_ALLOW_THREADS
    } else {
        value = zng_crc32(value, buf, (uint32_t)len);
    }

    return_value = PyLong_FromUnsignedLong(value & 0xffffffffU);
    PyBuffer_Release(&data);
    return return_value;
}


PyDoc_STRVAR(zlib_crc32_combine__doc__,
"crc32_combine($module, crc1, crc2, crc2_length /)\n"
"--\n"
"\n"
"Combine crc1 and crc2 into a new crc that is accurate for the combined data \n"
"blocks that crc1 and crc2 where calculated from.\n"
"\n"
"  crc1\n"
"    the first crc32 checksum\n"
"  crc2\n"
"    the second crc32 checksum\n"
"  crc2_length\n"
"    the lenght of the data block crc2 was calculated from\n"
);


#define ZLIB_CRC32_COMBINE_METHODDEF    \
    {"crc32_combine", (PyCFunction)(void(*)(void))zlib_crc32_combine, \
     METH_VARARGS, zlib_crc32_combine__doc__}

static PyObject *
zlib_crc32_combine(PyObject *module, PyObject *args) {
    uint32_t crc1 = 0;
    uint32_t crc2 = 0;
    Py_ssize_t crc2_length = 0;
    static char *format = "IIn:crc32_combine";
    if (PyArg_ParseTuple(args, format, &crc1, &crc2, &crc2_length) < 0) {
        return NULL;
    }
    return PyLong_FromUnsignedLong(
        zng_crc32_combine(crc1, crc2, crc2_length) & 0xFFFFFFFF);
}

typedef struct {
    PyObject_HEAD 
    uint8_t *buffer;
    uint32_t buffer_size;
    zng_stream zst;
    uint8_t is_initialised;
} ParallelCompress;

static void 
ParallelCompress_dealloc(ParallelCompress *self)
{
    PyMem_Free(self->buffer);
    if (self->is_initialised) {
        zng_deflateEnd(&self->zst);
    }
    Py_TYPE(self)->tp_free((PyObject *)self);
}

static PyObject *
ParallelCompress__new__(PyTypeObject *type, PyObject *args, PyObject *kwargs)
{
    Py_ssize_t buffer_size = 0;
    int level = Z_DEFAULT_COMPRESSION;
    static char *format = "n|i:ParallelCompress__new__";
    static char *kwarg_names[] = {"buffersize", "level", NULL};
    if (PyArg_ParseTupleAndKeywords(args, kwargs, format, kwarg_names, 
        &buffer_size, &level) < 0) {
            return NULL;
    }
    if (buffer_size > UINT32_MAX) {
        PyErr_Format(PyExc_ValueError, 
        "buffersize must be at most %zd, got %zd", 
        (Py_ssize_t)UINT32_MAX, buffer_size);
    }
    ParallelCompress *self = PyObject_New(ParallelCompress, type);
    if (self == NULL) {
        return PyErr_NoMemory();
    }
    self->buffer = NULL;
    self->zst.next_in = NULL;
    self->zst.avail_in = 0;
    self->zst.next_out = NULL;
    self->zst.avail_out = 0;
    self->zst.opaque = NULL;
    self->zst.zalloc = PyZlib_Malloc;
    self->zst.zfree = PyZlib_Free;
    self->is_initialised = 0;
    int err = zng_deflateInit2(&self->zst, level, DEFLATED, -MAX_WBITS, DEF_MEM_LEVEL, 
                               Z_DEFAULT_STRATEGY);
    switch (err) {
    case Z_OK:
        break;
    case Z_MEM_ERROR:
        PyErr_SetString(PyExc_MemoryError,
                        "Out of memory while compressing data");
        Py_DECREF(self);
        return NULL;
    case Z_STREAM_ERROR:
        PyErr_SetString(ZlibError, "Bad compression level");
        Py_DECREF(self);
        return NULL;
    default:
        zng_deflateEnd(&self->zst);
        zlib_error(self->zst, err, "while compressing data");
        Py_DECREF(self);
        return NULL;
    }
    self->is_initialised = 1;
    uint8_t *buffer = PyMem_Malloc(buffer_size);
    if (buffer == NULL) {
        Py_DECREF(self);
        return PyErr_NoMemory();
    }
    self->buffer = buffer;
    self->buffer_size = buffer_size;
    return (PyObject *)self;
}


PyDoc_STRVAR(ParallelCompress_compress_and_crc__doc__,
"compress_and_crc($self, data, zdict, /)\n"
"--\n"
"\n"
"Function specifically designed for use in parallel compression. Data is \n"
"compressed using deflate and Z_SYNC_FLUSH is used to ensure the block aligns\n"
"to a byte boundary. Also the CRC is calculated. This function is designed to \n"
"maximize the time spent outside the GIL\n"
"\n"
"  data\n"
"    bytes-like object containing the to be compressed data\n"
"  zdict\n"
"    last 32 bytes of the previous block\n"
);
#define PARALLELCOMPRESS_COMPRESS_AND_CRC_METHODDEF \
    { \
        "compress_and_crc", (PyCFunction)ParallelCompress_compress_and_crc, \
            METH_FASTCALL, ParallelCompress_compress_and_crc__doc__}

static PyObject *
ParallelCompress_compress_and_crc(ParallelCompress *self, 
                                  PyObject *const *args,
                                  Py_ssize_t nargs)
{
    if (nargs != 2) {
        PyErr_Format(
            PyExc_TypeError, 
            "compress_and_crc takes exactly 2 arguments, got %zd", 
            nargs);
        return NULL;
    }
    Py_buffer data;
    Py_buffer zdict;
    if (PyObject_GetBuffer(args[0], &data, PyBUF_SIMPLE) == -1) {
        return NULL;
    }
    if (PyObject_GetBuffer(args[1], &zdict, PyBUF_SIMPLE) == -1) {
        PyBuffer_Release(&data);
        return NULL;
    }

    if (data.len + zdict.len > UINT32_MAX) {
        PyErr_Format(PyExc_OverflowError, 
                     "Can only compress %d bytes of data", UINT32_MAX);
        goto error;
    }
    PyThreadState *_save;
    Py_UNBLOCK_THREADS
    int err = zng_deflateReset(&self->zst);
    if (err != Z_OK) {
        Py_BLOCK_THREADS;
        zlib_error(self->zst, err, "error resetting deflate state");
        goto error;
    }
    self->zst.avail_in = data.len;
    self->zst.next_in = data.buf;
    self->zst.next_out = self->buffer;
    self->zst.avail_out = self->buffer_size;
    err = zng_deflateSetDictionary(&self->zst, zdict.buf, zdict.len);
    if (err != Z_OK){
        Py_BLOCK_THREADS;
        zlib_error(self->zst, err, "error setting dictionary");
        goto error;
    }
    uint32_t crc = zng_crc32_z(0, data.buf, data.len);
    err = zng_deflate(&self->zst, Z_SYNC_FLUSH);
    Py_BLOCK_THREADS;

    if (err != Z_OK) {
        zlib_error(self->zst, err, "error setting dictionary");
        goto error;
    }
    if (self->zst.avail_out == 0) {
        PyErr_Format(
            PyExc_OverflowError,
            "Compressed output exceeds buffer size of %u", self->buffer_size
        );
        goto error;
    }
    if (self->zst.avail_in != 0) {
        PyErr_Format(
            PyExc_RuntimeError, 
            "Developer error input bytes are still available: %u. "
            "Please contact the developers by creating an issue at "
            "https://github.com/pycompression/python-isal/issues", 
            self->zst.avail_in);
        goto error;
    }
    PyObject *out_tup = PyTuple_New(2);
    PyObject *crc_obj = PyLong_FromUnsignedLong(crc);
    PyObject *out_bytes = PyBytes_FromStringAndSize(
        (char *)self->buffer, self->zst.next_out - self->buffer);
    if (out_bytes == NULL || out_tup == NULL || crc_obj == NULL) {
        Py_XDECREF(out_bytes); Py_XDECREF(out_tup); Py_XDECREF(crc_obj);
        goto error;
    }
    PyBuffer_Release(&data);
    PyBuffer_Release(&zdict); 
    PyTuple_SET_ITEM(out_tup, 0, out_bytes);
    PyTuple_SET_ITEM(out_tup, 1, crc_obj);
    return out_tup;
error:
    PyBuffer_Release(&data);
    PyBuffer_Release(&zdict); 
    return NULL;
}

static PyMethodDef ParallelCompress_methods[] = {
    PARALLELCOMPRESS_COMPRESS_AND_CRC_METHODDEF,
    {NULL},
};

static PyTypeObject ParallelCompress_Type = {
    .tp_name = "isal_zlib._ParallelCompress",
    .tp_basicsize = sizeof(ParallelCompress),
    .tp_doc = PyDoc_STR(
        "A reusable zstream and buffer fast parallel compression."),
    .tp_dealloc = (destructor)ParallelCompress_dealloc,
    .tp_new = ParallelCompress__new__,
    .tp_methods = ParallelCompress_methods,
};

PyDoc_STRVAR(zlib_compress__doc__,
"compress($module, data, /, level=Z_DEFAULT_COMPRESSION, wbits=MAX_WBITS)\n"
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
   
    return_value = zlib_decompress_impl(module, &data, wbits, bufsize);
    PyBuffer_Release(&data);
    return return_value;
}


PyDoc_STRVAR(zlib_compressobj__doc__,
"compressobj($module, /, level=Z_DEFAULT_COMPRESSION, method=DEFLATED,\n"
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

static PyTypeObject Comptype = {
    .tp_name = "zlib_ng._Compress",
    .tp_basicsize = sizeof(compobject),
    .tp_flags = Py_TPFLAGS_DEFAULT,
    .tp_dealloc = (destructor)Comp_dealloc,
    .tp_methods = comp_methods,
};

static PyTypeObject Decomptype = {
    .tp_name = "zlib_ng._Decompress",
    .tp_basicsize = sizeof(compobject),
    .tp_flags = Py_TPFLAGS_DEFAULT,
    .tp_dealloc = (destructor)Decomp_dealloc,
    .tp_methods = Decomp_methods,
    .tp_members = Decomp_members,
};

static PyTypeObject ZlibDecompressorType = {
    .tp_name = "zlib_ng._ZlibDecompressor",
    .tp_basicsize = sizeof(ZlibDecompressor),
    .tp_flags = Py_TPFLAGS_DEFAULT,
    .tp_members = ZlibDecompressor_members,
    .tp_dealloc = (destructor)ZlibDecompressor_dealloc,
    .tp_new = ZlibDecompressor__new__,
    .tp_doc = ZlibDecompressor__new____doc__,
    .tp_methods = ZlibDecompressor_methods,

};


#define GzipReader_HEADER 1
#define GzipReader_DEFLATE_BLOCK 2
#define GzipReader_TRAILER 3
#define GzipReader_NULL_BYTES 4

typedef struct _GzipReaderStruct {
    PyObject_HEAD
    uint8_t *input_buffer;
    size_t buffer_size;
    const uint8_t *current_pos; 
    const uint8_t *buffer_end; 
    int64_t _pos;
    int64_t _size;
    PyObject *fp;
    Py_buffer *memview;
    char stream_phase;
    char all_bytes_read;
    char closed;
    uint32_t crc;
    uint32_t stream_out;
    uint32_t _last_mtime;
    PyThread_type_lock lock;
    zng_stream zst;
} GzipReader;

static void GzipReader_dealloc(GzipReader *self) 
{
    if (self->memview == NULL) {
        PyMem_Free(self->input_buffer);
    } else {
        PyBuffer_Release(self->memview);
        PyMem_Free(self->memview);
    }
    Py_XDECREF(self->fp);
    PyThread_free_lock(self->lock);
    zng_inflateEnd(&self->zst);
    Py_TYPE(self)->tp_free(self);
}

PyDoc_STRVAR(GzipReader__new____doc__,
"_GzipReader(fp, /, buffersize=32*1024)\n"
"--\n"
"\n"
"Return a _GzipReader object.\n"
"\n"
"  fp\n"
"    can be a file-like binary IO object or a bytes-like object.\n"
"    For file-like objects _GzipReader's internal buffer is filled using \n"
"    fp's readinto method during reading. For bytes-like objects, the \n"
"    buffer protocol is used which allows _GzipReader to use the object \n"
"    itself as read buffer. "
"  buffersize\n"
"    Size of the internal buffer. Only used when fp is a file-like object. \n"
"    The buffer is automatically resized to fit the largest gzip header \n"
"    upon use of the _GzipReader object.\n"
);

static PyObject *
GzipReader__new__(PyTypeObject *type, PyObject *args, PyObject *kwargs)
{
    PyObject *fp = NULL;
    Py_ssize_t buffer_size = 32 * 1024;
    static char *keywords[] = {"fp", "buffersize", NULL};
    static char *format = "O|n:GzipReader";
    if (!PyArg_ParseTupleAndKeywords(
            args, kwargs, format, keywords, &fp, &buffer_size)) {
        return NULL;
    }
    if (buffer_size < 1) {
        PyErr_Format(
            PyExc_ValueError,
            "buffersize must be at least 1, got %zd", buffer_size
        );
        return NULL;
    }
    GzipReader *self = PyObject_New(GzipReader, type);
    if (PyObject_HasAttrString(fp, "read")) {
        self->memview = NULL;
        self->buffer_size = buffer_size;
        self->input_buffer = PyMem_Malloc(self->buffer_size);
        if (self->input_buffer == NULL) {
            Py_DECREF(self);
            return PyErr_NoMemory();
        }
        self->buffer_end = self->input_buffer;
        self->all_bytes_read = 0;
    } else {
        self->memview = PyMem_Malloc(sizeof(Py_buffer));
        if (self->memview == NULL) {
            return PyErr_NoMemory();
        }
        if (PyObject_GetBuffer(fp, self->memview, PyBUF_SIMPLE) < 0) {
            Py_DECREF(self);
            return NULL;
        }
        self->buffer_size = self->memview->len;
        self->input_buffer = self->memview->buf;
        self->buffer_end = self->input_buffer + self->buffer_size;
        self->all_bytes_read = 1;
    }
    self->current_pos = self->input_buffer;
    self->_pos = 0;
    self->_size = -1;
    Py_INCREF(fp);
    self->fp = fp;
    self->stream_phase = GzipReader_HEADER;
    self->closed = 0;
    self->_last_mtime = 0;
    self->crc = 0;
    self->lock = PyThread_allocate_lock();
    if (self->lock == NULL) {
        Py_DECREF(self);
        PyErr_SetString(PyExc_MemoryError, "Unable to allocate lock");
        return NULL;
    }
    self->zst.zalloc = PyZlib_Malloc;
    self->zst.zfree = PyZlib_Free;
    self->zst.next_in = NULL;
    self->zst.avail_in = 0;
    self->zst.opaque = NULL;
    int err = zng_inflateInit2(&(self->zst), -MAX_WBITS);
        switch (err) {
        case Z_OK:
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

static inline Py_ssize_t 
GzipReader_read_from_file(GzipReader *self) 
{

    const uint8_t *current_pos = self->current_pos;
    const uint8_t *buffer_end = self->buffer_end;
    size_t remaining = buffer_end - current_pos;
    if (remaining == self->buffer_size) {
        /* Buffer is full but a new read request was issued. This will be due 
           to the header being bigger than the header. Enlarge the buffer 
           to accommodate the hzip header.  */
        size_t new_buffer_size = self->buffer_size * 2;
        uint8_t *tmp_buffer = PyMem_Realloc(self->input_buffer, new_buffer_size);
        if (tmp_buffer == NULL) {
            PyErr_NoMemory();
            return -1;
        }
        self->input_buffer = tmp_buffer;
        self->buffer_size = new_buffer_size;
    } else if (remaining > 0) {
        memmove(self->input_buffer, current_pos, remaining);
    }
    uint8_t *input_buffer = self->input_buffer;
    current_pos = input_buffer;
    buffer_end = input_buffer + remaining;
    size_t read_in_size = self->buffer_size - remaining;
    PyObject *bufview = PyMemoryView_FromMemory(
        (char *)buffer_end, read_in_size, MEMORYVIEW_READINTO_FLAGS);
    if (bufview == NULL) {
        return -1;
    }
    PyObject *new_size_obj = PyObject_CallMethod(self->fp, "readinto", "O", bufview);
    Py_DECREF(bufview);
    if (new_size_obj == NULL) {
        return -1;
    }
    Py_ssize_t new_size = PyLong_AsSsize_t(new_size_obj);
    Py_DECREF(new_size_obj);
    if (new_size < 0) {
        return -1;
    }
    if (new_size == 0) {
        self->all_bytes_read = 1;
    }
    buffer_end += new_size;
    self->current_pos = current_pos;
    self->buffer_end = buffer_end;
    return 0;
}

#define FTEXT 1
#define FHCRC 2
#define FEXTRA 4
#define FNAME 8
#define FCOMMENT 16

static PyObject *BadGzipFile; // Import BadGzipFile error for consistency

static inline uint32_t load_u32_le(const void *address) {    
    #if PY_BIG_ENDIAN
    uint8_t *mem = address;
    return mem[0] | (mem[1] << 8) | (mem[2] << 16) | (mem[3] << 24);
    #else
    return *(uint32_t *)address;
    #endif
}

static inline uint16_t load_u16_le(const void *address) {
    #if PY_BIG_ENDIAN
    uint8_t *mem = address;
    return mem[0] | (mem[1] << 8) | (mem[2] << 16) | (mem[3] << 24);
    #else
    return *(uint16_t *)address;
    #endif
}

static Py_ssize_t 
GzipReader_read_into_buffer(GzipReader *self, uint8_t *out_buffer, size_t out_buffer_size)
{
    Py_ssize_t bytes_written = 0;
    /* Outer loop is the file read in loop */
    while (1) {
        const uint8_t *current_pos = self->current_pos;
        const uint8_t *buffer_end = self->buffer_end;
        /* Inner loop fills the out buffer, with multiple gzip blocks if 
           necessary. Allow escaping the GIL except when throwing errors. 
           This makes a big difference for BGZF format gzip blocks. 
           Threads are blocked when the loop is exited. */
        PyThreadState *_save;
        Py_UNBLOCK_THREADS
        while(1) {     
            switch(self->stream_phase) {
                size_t remaining; // Must be before labels.
                case GzipReader_HEADER:
                    remaining = buffer_end - current_pos;
                    if (remaining == 0 && self->all_bytes_read) {
                        // Reached EOF
                        self->_size = self->_pos;
                        self->current_pos = current_pos;
                        Py_BLOCK_THREADS;
                        return bytes_written;
                    } 
                    if ((remaining) < 10) {
                        break;
                    }
                    uint8_t magic1 = current_pos[0];
                    uint8_t magic2 = current_pos[1];
                    
                    if (!(magic1 == 0x1f && magic2 == 0x8b)) {
                        Py_BLOCK_THREADS;
                        PyObject *magic_obj = PyBytes_FromStringAndSize((char *)current_pos, 2);
                        PyErr_Format(BadGzipFile,
                            "Not a gzipped file (%R)", 
                            magic_obj);
                        Py_DECREF(magic_obj);
                        return -1;
                    };
                    uint8_t method = current_pos[2];
                    if (method != 8) {
                        Py_BLOCK_THREADS;
                        PyErr_SetString(BadGzipFile, "Unknown compression method");
                        return -1;
                    }
                    uint8_t flags = current_pos[3];
                    self->_last_mtime = load_u32_le(current_pos + 4);
                    // Skip XFL and header flag
                    const uint8_t *header_cursor = current_pos + 10;
                    if (flags & FEXTRA) {
                        // Read the extra field and discard it.
                        if (header_cursor + 2 >= buffer_end) {
                            break;
                        }
                        uint16_t flength = load_u16_le(header_cursor);
                        header_cursor += 2;
                        if (header_cursor + flength >= buffer_end) {
                            break;
                        }
                        header_cursor += flength;
                    }
                    if (flags & FNAME) {
                        header_cursor = memchr(header_cursor, 0, buffer_end - header_cursor);
                        if (header_cursor == NULL) {
                            break;
                        }
                        // skip over the 0 value;
                        header_cursor +=1;
                    }                 
                    if (flags & FCOMMENT) {
                        header_cursor = memchr(header_cursor, 0, buffer_end - header_cursor);
                        if (header_cursor == NULL) {
                            break;
                        }
                        // skip over the 0 value;
                        header_cursor +=1;
                    }
                    if (flags & FHCRC) {
                        if (header_cursor + 2 >= buffer_end) {
                            break;
                        }
                        uint16_t header_crc = load_u16_le(header_cursor);
                        uint16_t crc = zng_crc32_z(
                            0, current_pos, header_cursor - current_pos) & 0xFFFF;
                        if (header_crc != crc) {
                            Py_BLOCK_THREADS;
                            PyErr_Format(
                                BadGzipFile,
                                "Corrupted gzip header. Checksums do not "
                                "match: %04x != %04x",
                                crc, header_crc
                            );
                            return -1;
                        }
                        header_cursor += 2;
                    }
                    current_pos = header_cursor;
                    int reset_err = zng_inflateReset(&(self->zst));
                    if (reset_err != Z_OK) {
                        Py_BLOCK_THREADS;
                        zlib_error(self->zst, reset_err, "while initializing inflate stream.");
                        return -1;
                    }
                    self->crc = 0;
                    self->stream_phase = GzipReader_DEFLATE_BLOCK;
                case GzipReader_DEFLATE_BLOCK:
                    self->zst.next_in = current_pos;
                    self->zst.avail_in = Py_MIN((buffer_end -current_pos), UINT32_MAX);
                    self->zst.next_out = out_buffer;
                    self->zst.avail_out = Py_MIN(out_buffer_size, UINT32_MAX);
                    int ret;
                    ret = zng_inflate(&self->zst, Z_SYNC_FLUSH);
                    switch (ret) {
                        case Z_OK:
                        case Z_BUF_ERROR:
                        case Z_STREAM_END:
                            break;
                        case Z_MEM_ERROR:
                            Py_BLOCK_THREADS;
                            PyErr_SetString(PyExc_MemoryError,
                                            "Out of memory while decompressing data");
                            return -1;
                        default:
                            Py_BLOCK_THREADS;
                            zlib_error(self->zst, ret, "while decompressing data");
                            return -1;
                    }
                    size_t current_bytes_written = self->zst.next_out - out_buffer;
                    self->crc = zng_crc32_z(self->crc, out_buffer, current_bytes_written);
                    bytes_written += current_bytes_written;
                    self->_pos += current_bytes_written;
                    out_buffer = self->zst.next_out;
                    out_buffer_size -= current_bytes_written;
                    current_pos = self->zst.next_in;
                    if (!(ret == Z_STREAM_END)) {
                        if (out_buffer_size > 0) {
                            if (current_pos == buffer_end) {
                                // Need fresh bytes
                                break;
                            }
                            // Not all input data decompressed.
                            continue;
                        }
                        self->current_pos = current_pos;
                        Py_BLOCK_THREADS;
                        return bytes_written;
                    }
                    // Block done check trailer.
                    self->stream_phase = GzipReader_TRAILER;
                case GzipReader_TRAILER:
                    if (buffer_end - current_pos < 8) {
                        break;
                    }
                    uint32_t crc = load_u32_le(current_pos);
                    current_pos += 4;
                    if (crc != self->crc) {
                        Py_BLOCK_THREADS;
                        PyErr_Format(
                            BadGzipFile, 
                            "CRC check failed %u != %u", 
                            crc, self->crc
                        );
                        return -1;
                    }
                    uint32_t length = load_u32_le(current_pos);
                    current_pos += 4;
                    // ISIZE is the length of the original data modulo 2^32
                    if (length != (0xFFFFFFFFUL & self->zst.total_out)) {
                        Py_BLOCK_THREADS;
                        PyErr_SetString(BadGzipFile, "Incorrect length of data produced");
                        return -1;
                    }
                    self->stream_phase = GzipReader_NULL_BYTES;
                case GzipReader_NULL_BYTES:
                    // There maybe NULL bytes between gzip members
                    while (current_pos < buffer_end && *current_pos == 0) {
                        current_pos += 1;
                    }
                    if (current_pos == buffer_end) {
                        /* Not all NULL bytes may have been read, refresh the buffer.*/
                        break;
                    }
                    self->stream_phase = GzipReader_HEADER;
                    continue;
                default:
                    Py_UNREACHABLE();
            }
            break;
        }
        Py_BLOCK_THREADS;
        // If buffer_end is reached, nothing was returned and all bytes are 
        // read we have an EOFError.
        if (self->all_bytes_read) {
            if (self->stream_phase == GzipReader_NULL_BYTES) {
                self->_size = self->_pos;
                self->current_pos = current_pos;
                return bytes_written;
            }
            PyErr_SetString(
                PyExc_EOFError, 
                "Compressed file ended before the end-of-stream marker was reached"
            );
            return -1;
        }
        self->current_pos = current_pos;
        if (GzipReader_read_from_file(self) < 0) {
            return -1;
        }
    }
}

static PyObject *
GzipReader_readinto(GzipReader *self, PyObject *buffer_obj)
{
    Py_buffer view;
    if (PyObject_GetBuffer(buffer_obj, &view, PyBUF_SIMPLE) < 0) {
        return NULL;
    }
    uint8_t *buffer = view.buf;
    size_t buffer_size = view.len;
    ENTER_ZLIB(self);
    Py_ssize_t written_size = GzipReader_read_into_buffer(self, buffer, buffer_size);
    LEAVE_ZLIB(self);
    PyBuffer_Release(&view);
    if (written_size < 0) {
        return NULL;
    }
    return PyLong_FromSsize_t((Py_ssize_t)written_size);
}

static PyObject *
GzipReader_seek(GzipReader *self, PyObject *args, PyObject *kwargs) 
{
    Py_ssize_t offset;
    Py_ssize_t whence = SEEK_SET;
    static char *keywords[] = {"offset", "whence", NULL};
    static char format[] = {"n|n:GzipReader.seek"};
    if (PyArg_ParseTupleAndKeywords(args, kwargs, format, keywords, &offset, &whence) < 0) {
        return NULL;
    }
    // Recalculate offset as an absolute file position.
    if (whence == SEEK_SET) {
        ;
    } else if (whence == SEEK_CUR) {
        offset = self->_pos + offset;
    } else if (whence == SEEK_END) {
        // Seeking relative to EOF - we need to know the file's size.
        if (self->_size < 0) {
            size_t tmp_buffer_size = 8 * 1024;
            uint8_t *tmp_buffer = PyMem_Malloc(tmp_buffer_size);
            if (tmp_buffer == NULL) {
                return PyErr_NoMemory();
            }
            while (1) {
                /* Simply overwrite the tmp buffer over and over */
                Py_ssize_t written_bytes = GzipReader_read_into_buffer(
                    self, tmp_buffer, tmp_buffer_size
                );
                if (written_bytes < 0) {
                    PyMem_FREE(tmp_buffer);
                    return NULL;
                }
                if (written_bytes == 0) {
                    break;
                }
            }
            assert(self->_size >= 0);
            PyMem_Free(tmp_buffer);
        }
        offset = self->_size + offset;
    } else {
        PyErr_Format(
            PyExc_ValueError,
            "Invalid format for whence: %zd", whence
        );
        return NULL;
    }

    // Make it so that offset is the number of bytes to skip forward.
    if (offset < self->_pos) {
        PyObject *seek_result = PyObject_CallMethod(self->fp, "seek", "n", 0);
        if (seek_result == NULL) {
            return NULL;
        }
        self->stream_phase = GzipReader_HEADER;
        self->_pos = 0;
        self->all_bytes_read = 0;
        int ret = zng_inflateReset(&self->zst);
        if (ret != Z_OK) {
            zlib_error(self->zst, ret, "while seeking");
            return NULL;
        }
    } else {
        offset -= self->_pos;
    }
    
    // Read and discard data until we reach the desired position.
    if (offset > 0) {
        Py_ssize_t tmp_buffer_size = 8 * 1024;
        uint8_t *tmp_buffer = PyMem_Malloc(tmp_buffer_size);
        if (tmp_buffer == NULL) {
            return PyErr_NoMemory();
        }
        while (offset > 0) {
            Py_ssize_t bytes_written = GzipReader_read_into_buffer(
                self, tmp_buffer, Py_MIN(tmp_buffer_size, offset));
            if (bytes_written < 0) {
                PyMem_FREE(tmp_buffer);
                return NULL;
            }
            if (bytes_written == 0) {
                break;
            }
            offset -= bytes_written;
        }
        PyMem_Free(tmp_buffer);
    }
    return PyLong_FromLongLong(self->_pos);
}

static PyObject *
GzipReader_readall(GzipReader *self, PyObject *Py_UNUSED(ignore))
{
    /* Try to consume the entire buffer without too much overallocation */
    Py_ssize_t chunk_size = self->buffer_size * 4;
    /* Rather than immediately creating a list, read one chunk first and
       only create a list when more read operations are necessary. */
    PyObject *first_chunk = PyBytes_FromStringAndSize(NULL, chunk_size);
    if (first_chunk == NULL) {
        return NULL;
    }
    ENTER_ZLIB(self);
    Py_ssize_t written_size = GzipReader_read_into_buffer(
        self, (uint8_t *)PyBytes_AS_STRING(first_chunk), chunk_size);
    LEAVE_ZLIB(self);
    if (written_size < 0) {
        Py_DECREF(first_chunk);
        return NULL;
    }
    if (written_size < chunk_size) {
        if (_PyBytes_Resize(&first_chunk, written_size) < 0) {
            return NULL;
        }
        return first_chunk;
    }

    PyObject *chunk_list = PyList_New(1);
    if (chunk_list == NULL) {
        return NULL;
    }
    PyList_SET_ITEM(chunk_list, 0, first_chunk);
    while (1) {
        PyObject *chunk = PyBytes_FromStringAndSize(NULL, chunk_size);
        if (chunk == NULL) {
            Py_DECREF(chunk_list);
            return NULL;
        }
        ENTER_ZLIB(self);
        written_size = GzipReader_read_into_buffer(
            self, (uint8_t *)PyBytes_AS_STRING(chunk), chunk_size);
        LEAVE_ZLIB(self);
        if (written_size < 0) {
            Py_DECREF(chunk);
            Py_DECREF(chunk_list);
            return NULL;
        }
        if (written_size == 0) {
            Py_DECREF(chunk);
            break;
        }
        if (_PyBytes_Resize(&chunk, written_size) < 0) {
            Py_DECREF(chunk_list);
            return NULL;
        }
        int ret = PyList_Append(chunk_list, chunk);
        Py_DECREF(chunk);
        if (ret < 0) {
            Py_DECREF(chunk_list);
            return NULL;
        }
    }
    PyObject *empty_bytes = PyBytes_FromStringAndSize(NULL, 0);
    if (empty_bytes == NULL) {
        Py_DECREF(chunk_list);
        return NULL;
    }
    PyObject *ret = PyObject_CallMethod(empty_bytes, "join", "O", chunk_list);
    Py_DECREF(empty_bytes);
    Py_DECREF(chunk_list);
    return ret;
}

static PyObject *
GzipReader_read(GzipReader *self, PyObject *args) 
{
    Py_ssize_t size = -1;
    if (PyArg_ParseTuple(args, "|n:GzipReader.read", &size) < 0) {
        return NULL;
    }
    if (size < 0) {
        return GzipReader_readall(self, NULL);
    }
    if (size == 0) {
        return PyBytes_FromStringAndSize(NULL, 0);
    }
    Py_ssize_t answer_size = Py_MIN((Py_ssize_t)self->buffer_size * 10, size);
    PyObject *answer = PyBytes_FromStringAndSize(NULL, answer_size);
    if (answer == NULL) {
        return NULL;
    }
    ENTER_ZLIB(self);
    Py_ssize_t written_bytes = GzipReader_read_into_buffer(self, (uint8_t *)PyBytes_AS_STRING(answer), answer_size);
    LEAVE_ZLIB(self);
    if (written_bytes < 0) {
        Py_DECREF(answer);
        return NULL;
    }
    if (_PyBytes_Resize(&answer, written_bytes) < 0) {
        return NULL;
    }
    return answer;
}

static PyObject *
GzipReader_close(GzipReader *self, PyObject *Py_UNUSED(ignore)) {
    if (!self->closed) {
        self->closed = 1;
    }
    Py_RETURN_NONE;
}

static PyObject *
GzipReader_readable(GzipReader *self, PyObject *Py_UNUSED(ignore))
{
    Py_RETURN_TRUE;
}

static PyObject *
GzipReader_writable(GzipReader *self, PyObject *Py_UNUSED(ignore))
{
    Py_RETURN_TRUE;
}

static PyObject *
GzipReader_seekable(GzipReader *self, PyObject *Py_UNUSED(ignore)) {
    return PyObject_CallMethod(self->fp, "seekable", NULL);
}

static PyObject *
GzipReader_tell(GzipReader *self, PyObject *Py_UNUSED(ignore)) {
    return PyLong_FromLongLong(self->_pos);
}

static PyObject *
GzipReader_flush(GzipReader *self, PyObject *Py_UNUSED(ignore)) {
    Py_RETURN_NONE;
}

static PyObject *
GzipReader_get_last_mtime(GzipReader *self, void *Py_UNUSED(closure)) 
{
    if (self->_last_mtime) {
        return PyLong_FromUnsignedLong(self->_last_mtime);
    }
    Py_RETURN_NONE;
}

static PyObject *
GzipReader_get_closed(GzipReader *self, void *Py_UNUSED(closure)) 
{
    return PyBool_FromLong(self->closed);
}

static PyMethodDef GzipReader_methods[] = {
    {"readinto", (PyCFunction)GzipReader_readinto, METH_O, NULL},
    {"readable", (PyCFunction)GzipReader_readable, METH_NOARGS, NULL},
    {"writable", (PyCFunction)GzipReader_writable, METH_NOARGS, NULL},
    {"seekable", (PyCFunction)GzipReader_seekable, METH_NOARGS, NULL},
    {"tell", (PyCFunction)GzipReader_tell, METH_NOARGS, NULL},
    {"seek", (PyCFunction)GzipReader_seek, METH_VARARGS | METH_KEYWORDS, NULL},
    {"close", (PyCFunction)GzipReader_close, METH_NOARGS, NULL},
    {"readall", (PyCFunction)GzipReader_readall, METH_NOARGS, NULL},
    {"flush", (PyCFunction)GzipReader_flush, METH_NOARGS, NULL},
    {"read", (PyCFunction)GzipReader_read, METH_VARARGS, NULL},
    {NULL},
};

static PyGetSetDef GzipReader_properties[] = {
    {"closed", (getter)GzipReader_get_closed, NULL, NULL, NULL},
    {"_last_mtime", (getter)GzipReader_get_last_mtime, NULL, NULL, NULL},
    {NULL},
};

static PyTypeObject GzipReader_Type = {
    .tp_name = "isal_zlib._GzipReader",
    .tp_basicsize = sizeof(GzipReader),
    .tp_flags = Py_TPFLAGS_DEFAULT,
    .tp_dealloc = (destructor)GzipReader_dealloc,
    .tp_new = (newfunc)(GzipReader__new__),
    .tp_doc = GzipReader__new____doc__,
    .tp_methods = GzipReader_methods,
    .tp_getset = GzipReader_properties,
};


static PyMethodDef zlib_methods[] =
{
    ZLIB_ADLER32_METHODDEF,
    ZLIB_COMPRESS_METHODDEF,
    ZLIB_COMPRESSOBJ_METHODDEF,
    ZLIB_CRC32_METHODDEF,
    ZLIB_CRC32_COMBINE_METHODDEF,
    ZLIB_DECOMPRESS_METHODDEF,
    ZLIB_DECOMPRESSOBJ_METHODDEF,
    {NULL, NULL}
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

static struct PyModuleDef zlibmodule = {
    PyModuleDef_HEAD_INIT,
    .m_name = "zlib_ng",
    .m_doc = zlib_module_documentation,
    .m_methods = zlib_methods,
};

PyMODINIT_FUNC
PyInit_zlib_ng(void)
{
    PyObject *m, *ver;
    m = PyModule_Create(&zlibmodule);
    if (m == NULL) {
        return NULL;
    }
    if (PyType_Ready(&Comptype) < 0) {
        return NULL;
    }
    PyObject *Comptype_obj = (PyObject *)&Comptype;
    Py_INCREF(Comptype_obj);
    PyModule_AddObject(m, "_Compress", Comptype_obj);
    if (PyType_Ready(&Decomptype) < 0) {
        return NULL;
    }
    PyObject *Decomptype_obj = (PyObject *)&Decomptype;
    Py_INCREF(Decomptype_obj);
    PyModule_AddObject(m, "_Decompress", Decomptype_obj);
    if (PyType_Ready(&ZlibDecompressorType) < 0) {
        return NULL;
    }
    PyObject *ZlibDecompressorType_obj = (PyObject *)&ZlibDecompressorType;
    Py_INCREF(ZlibDecompressorType_obj);
    PyModule_AddObject(m, "_ZlibDecompressor", ZlibDecompressorType_obj);

    if (PyType_Ready(&GzipReader_Type) != 0) {
        return NULL;
    }
    Py_INCREF(&GzipReader_Type);
    if (PyModule_AddObject(m, "_GzipReader", (PyObject *)&GzipReader_Type) < 0) {
        return NULL;
    }

    if (PyType_Ready(&ParallelCompress_Type) != 0) {
        return NULL;
    }
    Py_INCREF(&ParallelCompress_Type);
    if (PyModule_AddObject(m, "_ParallelCompress", 
                           (PyObject *)&ParallelCompress_Type) < 0) {
        return NULL;
    }

    ZlibError = PyErr_NewException("zlib_ng.error", NULL, NULL);
    if (ZlibError == NULL) {
        return NULL; 
    } 
    Py_INCREF(ZlibError);
    PyModule_AddObject(m, "error", ZlibError);

    PyObject *gzip_module = PyImport_ImportModule("gzip");
    if (gzip_module == NULL) {
        return NULL;
    }

    BadGzipFile = PyObject_GetAttrString(gzip_module, "BadGzipFile");
    if (BadGzipFile == NULL) {
        return NULL;
    }
    Py_INCREF(BadGzipFile);

    PyModule_AddIntMacro(m, MAX_WBITS);
    PyModule_AddIntMacro(m, DEFLATED);
    PyModule_AddIntMacro(m, DEF_MEM_LEVEL);
    PyModule_AddIntMacro(m, DEF_BUF_SIZE);
    // compression levels
    PyModule_AddIntMacro(m, Z_NO_COMPRESSION);
    PyModule_AddIntMacro(m, Z_BEST_SPEED);
    PyModule_AddIntMacro(m, Z_BEST_COMPRESSION);
    PyModule_AddIntMacro(m, Z_DEFAULT_COMPRESSION);
    // compression strategies
    PyModule_AddIntMacro(m, Z_FILTERED);
    PyModule_AddIntMacro(m, Z_HUFFMAN_ONLY);
    PyModule_AddIntMacro(m, Z_RLE);
    PyModule_AddIntMacro(m, Z_FIXED);
    PyModule_AddIntMacro(m, Z_DEFAULT_STRATEGY);
    // allowed flush values
    PyModule_AddIntMacro(m, Z_NO_FLUSH);
    PyModule_AddIntMacro(m, Z_PARTIAL_FLUSH);
    PyModule_AddIntMacro(m, Z_SYNC_FLUSH);
    PyModule_AddIntMacro(m, Z_FULL_FLUSH);
    PyModule_AddIntMacro(m, Z_FINISH);
    PyModule_AddIntMacro(m, Z_BLOCK);
    PyModule_AddIntMacro(m, Z_TREES);
    ver = PyUnicode_FromString(ZLIBNG_VERSION);
    if (ver != NULL)
        PyModule_AddObject(m, "ZLIBNG_VERSION", ver);

    ver = PyUnicode_FromString(zlibng_version());
    if (ver != NULL)
        PyModule_AddObject(m, "ZLIBNG_RUNTIME_VERSION", ver);

    /* Add latest compatible zlib version */
    ver = PyUnicode_FromString("1.2.12");
    if (ver!= NULL) {
        PyModule_AddObject(m, "ZLIB_VERSION", ver);
        Py_INCREF(ver);
        PyModule_AddObject(m, "ZLIB_RUNTIME_VERSION", ver);
    }

    PyModule_AddStringConstant(m, "__version__", "1.0");

    return m;
}
