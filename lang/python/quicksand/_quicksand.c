/*  quicksand/_quicksand.c - thin C wrapper for the Quicksand shared library
 *
 *  Purpose
 *  --------
 *  • Load the Quicksand shared library at import time
 *  • Expose those functions to Python with proper argument/return
 *    conversion
 *  • Keep the GIL released while the library runs
 *
 *  Author:  Alec Graves
 *  Licence: AGPLv3
 */

#define _DARWIN_SOURCE
#define _GNU_SOURCE
#define PY_SSIZE_T_CLEAN

#include <Python.h>
#include <inttypes.h> /* for PRIu64 */
#include <limits.h>
#include <stddef.h>
#include <stdint.h>

#include <stdio.h>
#include <string.h>

#ifdef _WIN32
#include <windows.h>
#else
#include <dlfcn.h>
#endif

#include "quicksand.h" /* struct definitions */

/* ---------------- 1.  symbol pointers --------------------------------------*/

static void *lib_handle = NULL;

static int64_t (*p_quicksand_connect)(quicksand_connection **,
				      char *, int64_t, int64_t,
				      int64_t, void *) = NULL;
static void (*p_quicksand_disconnect)(quicksand_connection **,
				      void *) = NULL;
static void (*p_quicksand_delete)(char *, int64_t) = NULL;
static int64_t (*p_quicksand_write)(quicksand_connection *,
				    uint8_t *, int64_t) = NULL;
static int64_t (*p_quicksand_read)(quicksand_connection *,
				   uint8_t *, int64_t *) = NULL;
static uint64_t (*p_quicksand_now)(void) = NULL;
static double (*p_quicksand_ns)(uint64_t, uint64_t) = NULL;
static double (*p_quicksand_ns_calibrate)(double) = NULL;
static void (*p_quicksand_sleep)(double) = NULL;

#ifdef _WIN32
static void *
open_library(const char *path) {
	return (void *) LoadLibraryA(path);
}

static void *
lookup_symbol(const char *sym) {
	return (void *) GetProcAddress((HMODULE) lib_handle, sym);
}

static void
get_loader_error(char *buf, size_t buf_size) {
	DWORD code = GetLastError();

	if(!FormatMessageA(FORMAT_MESSAGE_FROM_SYSTEM
			   | FORMAT_MESSAGE_IGNORE_INSERTS,
			   NULL,
			   code,
			   0,
			   buf,
			   (DWORD) buf_size,
			   NULL)) {
		snprintf(buf, buf_size, "Windows loader error %lu",
			 (unsigned long) code);
	}
}
#else
static void *
open_library(const char *path) {
	return dlopen(path, RTLD_NOW | RTLD_LOCAL);
}

static void *
lookup_symbol(const char *sym) {
	return dlsym(lib_handle, sym);
}

static void
get_loader_error(char *buf, size_t buf_size) {
	const char *err = dlerror();

	if(!err) {
		err = "unknown dynamic loader error";
	}
	snprintf(buf, buf_size, "%s", err);
}
#endif

static int
build_sibling_path(PyObject *module, const char *lib_name,
		   char *buf, size_t buf_size) {
	PyObject *filename = NULL;
	PyObject *fs_path = NULL;
	const char *path = NULL;
	const char *slash = NULL;
	size_t dir_len = 0;

	filename = PyModule_GetFilenameObject(module);
	if(!filename) {
		return -1;
	}

	fs_path = PyUnicode_EncodeFSDefault(filename);
	Py_DECREF(filename);
	if(!fs_path) {
		return -1;
	}

	path = PyBytes_AsString(fs_path);
	if(!path) {
		Py_DECREF(fs_path);
		return -1;
	}

	slash = strrchr(path, '/');
#ifdef _WIN32
	{
		const char *backslash = strrchr(path, '\\');
		if(backslash && (!slash || backslash > slash)) {
			slash = backslash;
		}
	}
#endif

	if(!slash) {
		Py_DECREF(fs_path);
		return -1;
	}

	dir_len = (size_t) (slash - path);
	if(dir_len + 1 + strlen(lib_name) + 1 > buf_size) {
		Py_DECREF(fs_path);
		return -1;
	}

	memcpy(buf, path, dir_len);
	buf[dir_len] = '/';
	snprintf(buf + dir_len + 1, buf_size - dir_len - 1, "%s", lib_name);
	Py_DECREF(fs_path);
	return 0;
}

static int
load_quicksand_library(PyObject *module) {
	static const char *const candidates[] = {
#ifdef _WIN32
		"libquicksand.dll",
		"quicksand.dll",
#elif defined(__APPLE__)
		"libquicksand.dylib",
		"libquicksand.so",
#else
		"libquicksand.so",
#endif
	};
	char path_buf[4096];
	char err_buf[512];
	size_t i = 0;

	for(i = 0; i < sizeof(candidates) / sizeof(candidates[0]); i++) {
		if(build_sibling_path(module, candidates[i], path_buf,
				      sizeof(path_buf)) == 0) {
			lib_handle = open_library(path_buf);
			if(lib_handle) {
				return 0;
			}
		} else {
			PyErr_Clear();
		}

		lib_handle = open_library(candidates[i]);
		if(lib_handle) {
			return 0;
		}
	}

	get_loader_error(err_buf, sizeof(err_buf));
	PyErr_Format(PyExc_RuntimeError,
		     "Failed to load Quicksand shared library for this platform: %s",
		     err_buf);
	return -1;
}

/* ------------------------ 2.  load a single symbol -------------------------*/

static int
load_symbol(const char *sym, void **ptr) {
	char err_buf[512];
	void *addr = lookup_symbol(sym);
	if(!addr) {
		get_loader_error(err_buf, sizeof(err_buf));
		PyErr_Format(PyExc_RuntimeError,
			     "Failed to load symbol %s from Quicksand shared library: %s",
			     sym, err_buf);
		return -1;
	}
	*ptr = addr;
	return 0;
}

/* ----------- 3.  Create a PyCapsule holding a quicksand_connection* --------*/

static PyObject *
capsule_from_conn(quicksand_connection *conn) {
	/* No destructor - we explicitly disconnect from Python side. */
	return PyCapsule_New((void *) conn,
			     "quicksand_connection",
			     NULL);
}

/* -------------------- 4.  Python exposed API -------------------------------*/

/* 4.  Connect - correct kwlist and format string */
static PyObject *
py_quicksand_connect(PyObject *self, PyObject *args, PyObject *kwds) {
	static char *kwlist[] = {"topic",
				 "message_size",
				 "message_rate",
				 NULL};

	const char *topic;
	Py_ssize_t topic_len = -1; /* will stay -1 (NUL-terminated) */
	int64_t msg_sz = -1,
		msg_rate = -1;
	quicksand_connection *conn = NULL;
	int64_t rc;

	if(!PyArg_ParseTupleAndKeywords(args, kwds,
					"s|LL", kwlist,
					&topic, &msg_sz, &msg_rate)) {
		return NULL;
	}

	rc = p_quicksand_connect(&conn,
				 (char *) topic,
				 (int64_t) topic_len,
				 msg_sz,
				 msg_rate,
				 NULL); /* alloc = NULL => stdlib malloc */

	if(rc < 0) {
		PyErr_Format(PyExc_RuntimeError,
			     "quicksand_connect failed with code %lld",
			     (long long) rc);
		return NULL;
	}
	return Py_BuildValue("NLL", capsule_from_conn(conn),
			     (long long) conn->buffer->length,
			     (long long) conn->buffer->message_size);
}

static PyObject *
py_quicksand_delete(PyObject *self, PyObject *args, PyObject *kwds) {
	static char *kwlist[] = {"topic", NULL};

	const char *topic;
	Py_ssize_t topic_len = -1; /* will stay -1 (NUL-terminated) */

	if(!PyArg_ParseTupleAndKeywords(args, kwds,
					"s|", kwlist,
					&topic)) {
		return NULL;
	}

	p_quicksand_delete((char *) topic,
			   (int64_t) topic_len);

	Py_RETURN_NONE;
}

static PyObject *
py_quicksand_disconnect(PyObject *self, PyObject *arg) {
	quicksand_connection *conn;

	if(!PyCapsule_CheckExact(arg)) {
		PyErr_SetString(PyExc_TypeError,
				"expected a quicksand_connection capsule");
		return NULL;
	}

	conn = (quicksand_connection *) PyCapsule_GetPointer(arg,
							     "quicksand_connection");
	if(!conn) {
		return NULL; /* error already set */
	}

	p_quicksand_disconnect(&conn, NULL);
	Py_RETURN_NONE;
}

static PyObject *
py_quicksand_write(PyObject *self, PyObject *args) {
	PyObject *capsule, *msg_obj;
	quicksand_connection *conn;
	Py_buffer view;
	int64_t rc;

	if(!PyArg_ParseTuple(args, "OO", &capsule, &msg_obj)) {
		return NULL;
	}

	if(!PyCapsule_CheckExact(capsule)) {
		PyErr_Format(PyExc_TypeError,
			     "first argument must be a quicksand_connection capsule");
	}

	conn = (quicksand_connection *) PyCapsule_GetPointer(capsule,
							     "quicksand_connection");
	if(!conn) {
		return NULL;
	}

	if(PyObject_GetBuffer(msg_obj, &view, PyBUF_SIMPLE) < 0) {
		return NULL;
	}

	/* Release the GIL - the C call can block */
	rc = p_quicksand_write(conn,
			       (uint8_t *) view.buf,
			       (int64_t) view.len);

	PyBuffer_Release(&view);

	if(rc < 0) {
		PyErr_Format(PyExc_RuntimeError,
			     "quicksand_write failed with code %lld",
			     (long long) rc);
	} else {
		Py_RETURN_NONE;
	}
	return NULL;
}

static PyObject *
_py_quicksand_read(PyObject *self, PyObject *args, int latest) {
	PyObject *capsule, *buf_obj;
	quicksand_connection *conn;
	Py_buffer view;
	int64_t msg_sz = 0, rc;
	int64_t remaining;

	if(!PyArg_ParseTuple(args, "OO", &capsule, &buf_obj)) {
		return NULL;
	}

	if(!PyCapsule_CheckExact(capsule)) {
		PyErr_Format(PyExc_TypeError,
			     "first argument must be a quicksand_connection capsule");
	}

	conn = (quicksand_connection *) PyCapsule_GetPointer(capsule,
							     "quicksand_connection");
	if(!conn) {
		return NULL;
	}

	/* Request a mutable view - the caller must supply something writable */
	if(PyObject_GetBuffer(buf_obj, &view,
			      PyBUF_WRITABLE | PyBUF_SIMPLE)
	   < 0) {
		return NULL;
	}

	/* QuickSand expects the max size we are willing to accept.  The
       value inside view.len is a Py_ssize_t which we cast to int64_t.
       After the call, `msg_sz` contains the actual number of bytes
       written into the buffer. */
	msg_sz = (int64_t) view.len;

	// if latest, read only the latest available message if available
	if(latest) {
		if(conn->read_index < conn->buffer->index) {
			conn->read_index = conn->buffer->index - 1;
			rc = p_quicksand_read(conn,
					      (uint8_t *) view.buf,
					      &msg_sz);
		} else {
			rc = -1;
		}
	} else {
		rc = p_quicksand_read(conn,
				      (uint8_t *) view.buf,
				      &msg_sz);
	}


	PyBuffer_Release(&view);

	/* The library uses rc to indicate:
         rc > 0  - remaining messages in the ring.
         rc == -1 - no message was available at the moment
         rc < -1  - actual error code  */
	if(rc < 0) {
		if(rc == -1) {
			Py_RETURN_NONE;
		} else {
			PyErr_Format(PyExc_RuntimeError,
				     "quicksand_read failed with code %lld",
				     (long long) rc);
			return NULL;
		}
	}
	remaining = rc; /* guaranteed >0 here */

	/* `msg_sz` now contains how many bytes actually surface.  Slice the
       caller's buffer into a new `bytearray` that contains exactly those
       bytes.  We purposely avoid copying the full buffer - only the
       consumable part. */
	PyObject *payload = PyByteArray_FromStringAndSize(
			(char *) view.buf,
			(Py_ssize_t) msg_sz);
	if(!payload) {
		return NULL;
	}

	/* Return only the data. */
	return Py_BuildValue("N", payload);
}

static PyObject *
py_quicksand_read(PyObject *self, PyObject *args) {
	return _py_quicksand_read(self, args, 0);
}

static PyObject *
py_quicksand_read_latest(PyObject *self, PyObject *args) {
	return _py_quicksand_read(self, args, 1);
}

/* ------------------------------ misc helpers -------------------------------*/

static PyObject *
py_quicksand_remaining(PyObject *self, PyObject *args) {
	PyObject *capsule;
	quicksand_connection *conn;
	Py_buffer view;
	int64_t msg_sz = 0, rc;
	int64_t remaining;

	if(!PyArg_ParseTuple(args, "O", &capsule)) {
		return NULL;
	}

	if(!PyCapsule_CheckExact(capsule)) {
		PyErr_Format(PyExc_TypeError,
			     "first argument must be a quicksand_connection capsule");
	}

	conn = (quicksand_connection *) PyCapsule_GetPointer(capsule,
							     "quicksand_connection");
	if(!conn) {
		return NULL;
	}
	return PyLong_FromUnsignedLongLong(conn->buffer->index - conn->read_index);
}

/* ------------------------------ timing functions -------------------------------*/

static PyObject *
py_quicksand_now(PyObject *self, PyObject *Py_UNUSED(ignored)) {
	return PyLong_FromUnsignedLongLong(p_quicksand_now());
}

static PyObject *
py_quicksand_ns(PyObject *self, PyObject *args) {
	uint64_t end, start;

	if(!PyArg_ParseTuple(args, "KK", &end, &start)) {
		return NULL;
	}
	return PyFloat_FromDouble(p_quicksand_ns(end, start));
}

static PyObject *
py_quicksand_ns_elapsed(PyObject *self, PyObject *args) {
	uint64_t start;

	if(!PyArg_ParseTuple(args, "K", &start)) {
		return NULL;
	}
	return PyFloat_FromDouble(p_quicksand_ns(p_quicksand_now(), start));
}

static PyObject *
py_quicksand_sleep(PyObject *self, PyObject *args) {
	double ns;

	if(!PyArg_ParseTuple(args, "d", &ns)) {
		return NULL;
	}
	Py_BEGIN_ALLOW_THREADS;
	p_quicksand_sleep(ns);
	Py_END_ALLOW_THREADS;

	Py_RETURN_NONE;
}

/* --------------------5.  Module method table -------------------------------*/

static PyMethodDef QuickSandMethods[] = {
		{"connect", (PyCFunction) py_quicksand_connect,
		 METH_VARARGS | METH_KEYWORDS,
		 "Connect to a quicksand ring buffer and return a capsule."},
		{"disconnect", py_quicksand_disconnect,
		 METH_O, "Disconnect and free a connection capsule."},
		{"delete", (PyCFunction) py_quicksand_delete,
		 METH_VARARGS | METH_KEYWORDS, "Remove a shared memory buffer for future connections."},
		{"write", py_quicksand_write,
		 METH_VARARGS, "Write a bytes-like object to the ring buffer."},
		{"read", py_quicksand_read,
		 METH_VARARGS, "Read a message into a mutable buffer, returning (msg, remaining)."},
		{"read_latest", py_quicksand_read_latest,
		 METH_VARARGS, "Read latest message into a mutable buffer, returning (msg, remaining)."},
		{"remaining", py_quicksand_remaining,
		 METH_VARARGS, "Return the number of unread messages"},
		{"now", py_quicksand_now, METH_NOARGS,
		 "Monotonic timestamp (raw cycles)."},
		{"ns", py_quicksand_ns, METH_VARARGS,
		 "Nanoseconds between two timestamps."},
		{"ns_elapsed", py_quicksand_ns_elapsed, METH_VARARGS,
		 "Nanoseconds since the provided timestamp."},
		{"sleep", py_quicksand_sleep, METH_VARARGS,
		 "Sleep for the given number of nanoseconds."},
		{NULL, NULL, 0, NULL}};

/* -------------------------- 6.  Module definition --------------------------*/

static struct PyModuleDef quicksandmodule = {
		PyModuleDef_HEAD_INIT,
		"quicksand._quicksand",		      /* full name */
		"Thin C wrapper for the Quicksand shared library", /* doc */
		-1,				      /* per-interpreter state */
		QuickSandMethods,
		NULL, NULL, NULL, NULL};

/* ----------------------------- 7.  Module init -----------------------------*/

PyMODINIT_FUNC
PyInit__quicksand(void) {
	PyObject *m = PyModule_Create(&quicksandmodule);
	if(!m) {
		return NULL;
	}

	if(load_quicksand_library(m) < 0) {
		Py_DECREF(m);
		return NULL;
	}

	/* Resolve all required symbols and abort if any is missing. */
	if(load_symbol("quicksand_connect", (void **) &p_quicksand_connect) < 0) {
		Py_DECREF(m);
		return NULL;
	}
	if(load_symbol("quicksand_disconnect", (void **) &p_quicksand_disconnect) < 0) {
		Py_DECREF(m);
		return NULL;
	}
	if(load_symbol("quicksand_delete", (void **) &p_quicksand_delete) < 0) {
		Py_DECREF(m);
		return NULL;
	}
	if(load_symbol("quicksand_write", (void **) &p_quicksand_write) < 0) {
		Py_DECREF(m);
		return NULL;
	}
	if(load_symbol("quicksand_read", (void **) &p_quicksand_read) < 0) {
		Py_DECREF(m);
		return NULL;
	}
	if(load_symbol("quicksand_now", (void **) &p_quicksand_now) < 0) {
		Py_DECREF(m);
		return NULL;
	}
	if(load_symbol("quicksand_ns", (void **) &p_quicksand_ns) < 0) {
		Py_DECREF(m);
		return NULL;
	}
	if(load_symbol("quicksand_ns_calibrate", (void **) &p_quicksand_ns_calibrate) < 0) {
		Py_DECREF(m);
		return NULL;
	}
	if(load_symbol("quicksand_sleep", (void **) &p_quicksand_sleep) < 0) {
		Py_DECREF(m);
		return NULL;
	}

	return m;
}
