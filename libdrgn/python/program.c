// Copyright (c) Meta Platforms, Inc. and affiliates.
// SPDX-License-Identifier: LGPL-2.1-or-later

#include "drgnpy.h"
#include "../bitops.h"
#include "../error.h"
#include "../hash_table.h"
#include "../linux_kernel.h"
#include "../log.h"
#include "../program.h"
#include "../string_builder.h"
#include "../symbol.h"
#include "../util.h"
#include "../vector.h"

DEFINE_HASH_SET_FUNCTIONS(pyobjectp_set, ptr_key_hash_pair, scalar_key_eq);

static PyObject *percent_s;
static PyObject *logging_StreamHandler;
static PyObject *logger;
static PyObject *logger_log;

static void drgnpy_log_fn(struct drgn_program *prog, void *arg,
			  enum drgn_log_level level, const char *format,
			  va_list ap, struct drgn_error *err)
{
	STRING_BUILDER(sb);
	if (!string_builder_vappendf(&sb, format, ap))
		return;
	if (err && !string_builder_append_error(&sb, err))
		return;

	PyGILState_guard();
	PyObject *ret = PyObject_CallFunction(logger_log, "iOs#",
					      (level + 1) * 10, percent_s,
					      sb.str ? sb.str : "",
					      (Py_ssize_t)sb.len);
	if (ret)
		Py_DECREF(ret);
	else
		PyErr_WriteUnraisable(logger_log);
}

static int get_logging_status(int *log_level_ret, bool *enable_progress_bar_ret)
{
	// We don't use getEffectiveLevel() because that doesn't take
	// logging.disable() into account.
	int level;
	for (level = 0; level < DRGN_LOG_NONE; level++) {
		_cleanup_pydecref_ PyObject *enabled =
			PyObject_CallMethod(logger, "isEnabledFor", "i",
					    (level + 1) * 10);
		if (!enabled)
			return -1;
		int ret = PyObject_IsTrue(enabled);
		if (ret < 0)
			return -1;
		if (ret)
			break;
	}

	*log_level_ret = level;

	if (level > DRGN_LOG_WARNING || !isatty(STDERR_FILENO)) {
		*enable_progress_bar_ret = false;
		return 0;
	}

	PyObject *current_logger = logger;
	_cleanup_pydecref_ PyObject *logger_to_decref = NULL;
	do {
		_cleanup_pydecref_ PyObject *handlers =
			PyObject_GetAttrString(current_logger, "handlers");
		if (!handlers)
			return -1;

		Py_ssize_t size = PySequence_Size(handlers);
		if (size < 0)
			return -1;

		for (Py_ssize_t i = 0; i < size; i++) {
			_cleanup_pydecref_ PyObject *handler =
				PySequence_GetItem(handlers, i);
			if (!handler)
				return -1;

			int r = PyObject_IsInstance(handler,
						    logging_StreamHandler);
			if (r < 0)
				return -1;
			if (!r)
				continue;

			_cleanup_pydecref_ PyObject *stream =
				PyObject_GetAttrString(handler, "stream");
			if (!stream)
				return -1;

			_cleanup_pydecref_ PyObject *fd_obj =
				PyObject_CallMethod(stream, "fileno", NULL);
			if (!fd_obj) {
				// Ignore AttributeError,
				// io.UnsupportedOperation, etc.
				if (PyErr_ExceptionMatches(PyExc_Exception)) {
					PyErr_Clear();
					continue;
				} else {
					return -1;
				}
			}

			long fd = PyLong_AsLong(fd_obj);
			if (fd == -1 && PyErr_Occurred())
				return -1;

			if (fd == STDERR_FILENO) {
				*enable_progress_bar_ret = true;
				return 0;
			}
		}

		_cleanup_pydecref_ PyObject *propagate =
			PyObject_GetAttrString(current_logger, "propagate");
		if (!propagate)
			return -1;
		int ret = PyObject_IsTrue(propagate);
		if (ret < 0)
			return -1;
		if (!ret)
			break;

		Py_XDECREF(logger_to_decref);
		logger_to_decref = PyObject_GetAttrString(current_logger,
							  "parent");
		if (!logger_to_decref)
			return -1;
		current_logger = logger_to_decref;
	} while (current_logger != Py_None);

	*enable_progress_bar_ret = false;
	return 0;
}

// This is slightly heinous. We need to sync the Python logging configuration
// with libdrgn, but the Python log level and handlers can change at any time,
// and there are no APIs to be notified of this.
//
// To sync the log level, we monkey patch logger._cache.clear() to update the
// libdrgn log level on every live program.
//
// We also check handlers in that monkey patch, which isn't the right place to
// hook but should work in practice in most cases.
static int cached_log_level;
static bool cached_enable_progress_bar;
static struct pyobjectp_set programs = HASH_TABLE_INIT;

static int cache_logging_status(void)
{
	return get_logging_status(&cached_log_level,
				  &cached_enable_progress_bar);
}

static PyObject *LoggerCacheWrapper_clear(PyObject *self)
{
	PyDict_Clear(self);
	if (!pyobjectp_set_empty(&programs)) {
		if (cache_logging_status())
			return NULL;
		hash_table_for_each(pyobjectp_set, it, &programs) {
			Program *prog = (Program *)*it.entry;
			drgn_program_set_log_level(&prog->prog,
						   cached_log_level);
			drgn_program_set_progress_file(&prog->prog,
						       cached_enable_progress_bar
						       ? stderr : NULL);
		}
	}
	Py_RETURN_NONE;
}

static PyMethodDef LoggerCacheWrapper_methods[] = {
	{"clear", (PyCFunction)LoggerCacheWrapper_clear, METH_NOARGS},
	{},
};

static PyTypeObject LoggerCacheWrapper_type = {
	PyVarObject_HEAD_INIT(NULL, 0)
	.tp_name = "_drgn._LoggerCacheWrapper",
	.tp_methods = LoggerCacheWrapper_methods,
};

static int init_logger_cache_wrapper(void)
{
	LoggerCacheWrapper_type.tp_base = &PyDict_Type;
	if (PyType_Ready(&LoggerCacheWrapper_type))
		return -1;
	_cleanup_pydecref_ PyObject *cache_wrapper =
		PyObject_CallFunction((PyObject *)&LoggerCacheWrapper_type,
				      NULL);
	if (!cache_wrapper)
		return -1;
	return PyObject_SetAttrString(logger, "_cache", cache_wrapper);
}

static int Program_init_logging(Program *prog)
{
	// The cache is only maintained while there are live programs, so if
	// this is the only program, we need to update the cache.
	if (pyobjectp_set_empty(&programs) && cache_logging_status())
		return -1;

	PyObject *obj = (PyObject *)prog;
	if (pyobjectp_set_insert(&programs, &obj, NULL) < 0)
		return -1;
	drgn_program_set_log_callback(&prog->prog, drgnpy_log_fn, NULL);
	drgn_program_set_log_level(&prog->prog, cached_log_level);
	drgn_program_set_progress_file(&prog->prog,
				       cached_enable_progress_bar ? stderr : NULL);
	return 0;
}

static void Program_deinit_logging(Program *prog)
{
	PyObject *obj = (PyObject *)prog;
	pyobjectp_set_delete(&programs, &obj);
}

int init_logging(void)
{
	percent_s = PyUnicode_InternFromString("%s");
	if (!percent_s)
		return -1;

	_cleanup_pydecref_ PyObject *logging = PyImport_ImportModule("logging");
	if (!logging)
		return -1;
	logging_StreamHandler = PyObject_GetAttrString(logging,
						       "StreamHandler");
	if (!logging_StreamHandler)
		return -1;
	logger = PyObject_CallMethod(logging, "getLogger", "s", "drgn");
	if (!logger)
		return -1;
	logger_log = PyObject_GetAttrString(logger, "log");
	if (!logger_log)
		return -1;

	return init_logger_cache_wrapper();
}

int Program_hold_object(Program *prog, PyObject *obj)
{
	int ret = pyobjectp_set_insert(&prog->objects, &obj, NULL);
	if (ret > 0) {
		Py_INCREF(obj);
		ret = 0;
	} else if (ret < 0) {
		PyErr_NoMemory();
	}
	return ret;
}

bool Program_hold_reserve(Program *prog, size_t n)
{
	if (!pyobjectp_set_reserve(&prog->objects,
				   pyobjectp_set_size(&prog->objects) + n)) {
		PyErr_NoMemory();
		return false;
	}
	return true;
}

int Program_type_arg(Program *prog, PyObject *type_obj, bool can_be_none,
		     struct drgn_qualified_type *ret)
{
	struct drgn_error *err;

	if (PyObject_TypeCheck(type_obj, &DrgnType_type)) {
		if (DrgnType_prog((DrgnType *)type_obj) != prog) {
			PyErr_SetString(PyExc_ValueError,
					"type is from different program");
			return -1;
		}
		ret->type = ((DrgnType *)type_obj)->type;
		ret->qualifiers = ((DrgnType *)type_obj)->qualifiers;
	} else if (PyUnicode_Check(type_obj)) {
		const char *name;

		name = PyUnicode_AsUTF8(type_obj);
		if (!name)
			return -1;
		err = drgn_program_find_type(&prog->prog, name, NULL, ret);
		if (err) {
			set_drgn_error(err);
			return -1;
		}
	} else if (can_be_none && type_obj == Py_None) {
		ret->type = NULL;
		ret->qualifiers = 0;
	} else {
		PyErr_SetString(PyExc_TypeError,
				can_be_none ?
				"type must be Type, str, or None" :
				"type must be Type or str");
		return -1;
	}
	return 0;
}

void *drgn_begin_blocking(void)
{
	PyThreadState *state = PyThreadState_GetUnchecked();
	if (state)
		PyEval_ReleaseThread(state);
	return state;
}

void drgn_end_blocking(void *state)
{
	if (state)
		PyEval_RestoreThread(state);
}

static Program *Program_new_impl(const struct drgn_platform *platform)
{
	_cleanup_pydecref_ PyObject *cache = PyDict_New();
	if (!cache)
		return NULL;
	_cleanup_pydecref_ PyObject *config = PyDict_New();
	if (!config)
		return NULL;

	_cleanup_pydecref_ Program *prog = call_tp_alloc(Program);
	if (!prog)
		return NULL;
	prog->cache = no_cleanup_ptr(cache);
	prog->config = no_cleanup_ptr(config);
	pyobjectp_set_init(&prog->objects);
	drgn_program_init(&prog->prog, platform);
	if (Program_init_logging(prog))
		return NULL;
	return_ptr(prog);
}

static Program *Program_new(PyTypeObject *subtype, PyObject *args,
			    PyObject *kwds)
{
	static char *keywords[] = { "platform", "vmcoreinfo", NULL };
	PyObject *platform_obj = NULL;
	const char *vmcoreinfo = NULL;
	Py_ssize_t vmcoreinfo_size;
	if (!PyArg_ParseTupleAndKeywords(args, kwds, "|O$z#:Program", keywords,
					 &platform_obj, &vmcoreinfo,
					 &vmcoreinfo_size))
		return NULL;

	struct drgn_platform *platform;
	if (!platform_obj || platform_obj == Py_None) {
		platform = NULL;
	} else if (PyObject_TypeCheck(platform_obj, &Platform_type)) {
		platform = ((Platform *)platform_obj)->platform;
	} else {
		PyErr_SetString(PyExc_TypeError,
				"platform must be Platform or None");
		return NULL;
	}
	_cleanup_pydecref_ Program *prog = Program_new_impl(platform);
	if (!prog)
		return NULL;
	if (vmcoreinfo) {
		struct drgn_error *err = drgn_program_parse_vmcoreinfo(
			&prog->prog, vmcoreinfo, vmcoreinfo_size);
		if (err)
			return set_drgn_error(err);
	}
	return_ptr(prog);
}

LIBDRGN_PUBLIC struct drgn_error *
drgn_program_create(const struct drgn_platform *platform,
		    struct drgn_program **ret)
{
	bool success;
	drgn_initialize_python_guard(&success);
	if (!success)
		return drgn_error_from_python();
	Program *prog = Program_new_impl(platform);
	if (!prog)
		return drgn_error_from_python();
	*ret = &prog->prog;
	return NULL;
}

LIBDRGN_PUBLIC void drgn_program_destroy(struct drgn_program *prog)
{
	if (prog) {
		PyGILState_guard();
		Py_DECREF(container_of(prog, Program, prog));
	}
}

static void Program_dealloc(Program *self)
{
	PyObject_GC_UnTrack(self);
	Program_deinit_logging(self);
	drgn_program_deinit(&self->prog);
	hash_table_for_each(pyobjectp_set, it, &self->objects)
		Py_DECREF(*it.entry);
	pyobjectp_set_deinit(&self->objects);
	Py_XDECREF(self->config);
	Py_XDECREF(self->cache);
	Py_TYPE(self)->tp_free((PyObject *)self);
}

static int Program_traverse(Program *self, visitproc visit, void *arg)
{
	hash_table_for_each(pyobjectp_set, it, &self->objects)
		Py_VISIT(*it.entry);
	Py_VISIT(self->cache);
	Py_VISIT(self->config);
	return 0;
}

static int Program_clear(Program *self)
{
	hash_table_for_each(pyobjectp_set, it, &self->objects)
		Py_DECREF(*it.entry);
	pyobjectp_set_deinit(&self->objects);
	pyobjectp_set_init(&self->objects);
	Py_CLEAR(self->cache);
	Py_CLEAR(self->config);
	return 0;
}

static struct drgn_error *py_memory_read_fn(void *buf, uint64_t address,
					    size_t count, uint64_t offset,
					    void *arg, bool physical)
{
	struct drgn_error *err;

	PyGILState_guard();

	_cleanup_pydecref_ PyObject *ret =
		PyObject_CallFunction(arg, "KKKO", (unsigned long long)address,
				      (unsigned long long)count,
				      (unsigned long long)offset,
				      physical ? Py_True : Py_False);
	if (!ret)
		return drgn_error_from_python();
	Py_buffer view;
	if (PyObject_GetBuffer(ret, &view, PyBUF_SIMPLE) == -1)
		return drgn_error_from_python();
	if (view.len != count) {
		PyErr_Format(PyExc_ValueError,
			     "memory read callback returned buffer of length %zd (expected %zu)",
			     view.len, count);
		err = drgn_error_from_python();
		goto out;
	}
	memcpy(buf, view.buf, count);
	err = NULL;
out:
	PyBuffer_Release(&view);
	return err;
}

static PyObject *Program_add_memory_segment(Program *self, PyObject *args,
					    PyObject *kwds)
{
	static char *keywords[] = {
		"address", "size", "read_fn", "physical", NULL,
	};
	struct drgn_error *err;
	struct index_arg address = {};
	struct index_arg size = {};
	PyObject *read_fn;
	int physical = 0;

	if (!PyArg_ParseTupleAndKeywords(args, kwds,
					 "O&O&O|p:add_memory_segment", keywords,
					 index_converter, &address,
					 index_converter, &size, &read_fn,
					 &physical))
	    return NULL;

	if (!PyCallable_Check(read_fn)) {
		PyErr_SetString(PyExc_TypeError, "read_fn must be callable");
		return NULL;
	}

	if (Program_hold_object(self, read_fn) == -1)
		return NULL;
	err = drgn_program_add_memory_segment(&self->prog, address.uvalue,
					      size.uvalue, py_memory_read_fn,
					      read_fn, physical);
	if (err)
		return set_drgn_error(err);
	Py_RETURN_NONE;
}

static struct drgn_error *
py_debug_info_find_fn(struct drgn_module * const *modules, size_t num_modules,
		      void *arg)
{
	PyGILState_guard();

	_cleanup_pydecref_ PyObject *modules_list = PyList_New(num_modules);
	if (!modules_list)
		return drgn_error_from_python();
	for (size_t i = 0; i < num_modules; i++) {
		PyObject *module_obj = Module_wrap(modules[i]);
		if (!module_obj)
			return drgn_error_from_python();
		PyList_SET_ITEM(modules_list, i, module_obj);
	}
	_cleanup_pydecref_ PyObject *obj =
		PyObject_CallOneArg(arg, modules_list);
	if (!obj)
		return drgn_error_from_python();
	return NULL;
}

static inline struct drgn_error *
py_type_find_fn_common(PyObject *type_obj, void *arg,
		       struct drgn_qualified_type *ret)
{
	if (!PyObject_TypeCheck(type_obj, &DrgnType_type)) {
		PyErr_SetString(PyExc_TypeError,
				"type find callback must return Type or None");
		return drgn_error_from_python();
	}
	// This check is also done in libdrgn, but we need it here because if
	// the type isn't from this program, then there's no guarantee that it
	// will remain valid after we decrement its reference count.
	if (DrgnType_prog((DrgnType *)type_obj)
	    != (Program *)PyTuple_GET_ITEM(arg, 0)) {
		PyErr_SetString(PyExc_ValueError,
				"type find callback returned type from wrong program");
		return drgn_error_from_python();
	}
	ret->type = ((DrgnType *)type_obj)->type;
	ret->qualifiers = ((DrgnType *)type_obj)->qualifiers;
	return NULL;
}

static struct drgn_error *py_type_find_fn(uint64_t kinds, const char *name,
					  size_t name_len, const char *filename,
					  void *arg,
					  struct drgn_qualified_type *ret)
{
	PyGILState_guard();

	_cleanup_pydecref_ PyObject *name_obj =
		PyUnicode_FromStringAndSize(name, name_len);
	if (!name_obj)
		return drgn_error_from_python();

	_cleanup_pydecref_ PyObject *kinds_obj = TypeKindSet_wrap(kinds);
	if (!kinds_obj)
		return drgn_error_from_python();
	_cleanup_pydecref_ PyObject *type_obj =
		PyObject_CallFunction(PyTuple_GET_ITEM(arg, 1), "OOOs",
				      PyTuple_GET_ITEM(arg, 0), kinds_obj,
				      name_obj, filename);
	if (!type_obj)
		return drgn_error_from_python();
	if (type_obj == Py_None)
		return &drgn_not_found;
	return py_type_find_fn_common(type_obj, arg, ret);
}

// Old version for add_type_finder().
static struct drgn_error *py_type_find_fn_old(uint64_t kinds, const char *name,
					      size_t name_len,
					      const char *filename, void *arg,
					      struct drgn_qualified_type *ret)
{
	PyGILState_guard();

	_cleanup_pydecref_ PyObject *name_obj =
		PyUnicode_FromStringAndSize(name, name_len);
	if (!name_obj)
		return drgn_error_from_python();

	int kind;
	for_each_bit(kind, kinds) {
		_cleanup_pydecref_ PyObject *
			kind_obj = PyObject_CallFunction(TypeKind_class, "i",
							 kind);
		if (!kind_obj)
			return drgn_error_from_python();
		_cleanup_pydecref_ PyObject *type_obj =
			PyObject_CallFunction(PyTuple_GET_ITEM(arg, 1), "OOs",
					      kind_obj, name_obj, filename);
		if (!type_obj)
			return drgn_error_from_python();
		if (type_obj == Py_None)
			continue;
		return py_type_find_fn_common(type_obj, arg, ret);
	}
	return &drgn_not_found;
}

static struct drgn_error *py_object_find_fn(const char *name, size_t name_len,
					    const char *filename,
					    enum drgn_find_object_flags flags,
					    void *arg, struct drgn_object *ret)
{
	PyGILState_guard();

	_cleanup_pydecref_ PyObject *name_obj =
		PyUnicode_FromStringAndSize(name, name_len);
	if (!name_obj)
		return drgn_error_from_python();
	_cleanup_pydecref_ PyObject *flags_obj =
		PyObject_CallFunction(FindObjectFlags_class, "i", (int)flags);
	if (!flags_obj)
		return drgn_error_from_python();
	_cleanup_pydecref_ PyObject *obj =
		PyObject_CallFunction(arg, "OOOs",
				      container_of(drgn_object_program(ret), Program, prog),
				      name_obj, flags_obj, filename);
	if (!obj)
		return drgn_error_from_python();
	if (obj == Py_None)
		return &drgn_not_found;
	if (!PyObject_TypeCheck(obj, &DrgnObject_type)) {
		PyErr_SetString(PyExc_TypeError,
				"object find callback must return Object or None");
		return drgn_error_from_python();
	}

	return drgn_object_copy(ret, &((DrgnObject *)obj)->obj);
}

static struct drgn_error *
py_symbol_find_fn(const char *name, uint64_t addr,
		  enum drgn_find_symbol_flags flags, void *arg,
		  struct drgn_symbol_result_builder *builder)
{
	// Fast path for SymbolIndex: don't bother converting to and from Python
	// types, as this is a C finder. Use Py_TYPE and pointer comparison
	// directly here to avoid needing to take the GIL for
	// PyObject_TypeCheck(). SymbolIndex cannot be subclassed, so the logic
	// for subclass checking is unnecessary anyway.
	if (Py_TYPE(PyTuple_GET_ITEM(arg, 1)) == &SymbolIndex_type) {
		SymbolIndex *ix = (SymbolIndex *)PyTuple_GET_ITEM(arg, 1);
		return drgn_symbol_index_find(name, addr, flags, &ix->index, builder);
	}

	PyGILState_guard();

	_cleanup_pydecref_ PyObject *name_obj = NULL;
	if (flags & DRGN_FIND_SYMBOL_NAME) {
		name_obj = PyUnicode_FromString(name);
		if (!name_obj)
			return drgn_error_from_python();
	} else {
		name_obj = Py_None;
		Py_INCREF(name_obj);
	}

	_cleanup_pydecref_ PyObject *address_obj = NULL;
	if (flags & DRGN_FIND_SYMBOL_ADDR) {
		address_obj = PyLong_FromUnsignedLong(addr);
		if (!address_obj)
			return drgn_error_from_python();
	} else {
		address_obj = Py_None;
		Py_INCREF(address_obj);
	}

	_cleanup_pydecref_ PyObject *one_obj = PyBool_FromLong(flags & DRGN_FIND_SYMBOL_ONE);

	_cleanup_pydecref_ PyObject *tmp =
		PyObject_CallFunction(PyTuple_GET_ITEM(arg, 1), "OOOO",
				      PyTuple_GET_ITEM(arg, 0), name_obj,
				      address_obj, one_obj);
	if (!tmp)
		return drgn_error_from_python();

	_cleanup_pydecref_ PyObject *obj =
		PySequence_Fast(tmp, "symbol finder must return a sequence");
	if (!obj)
		return drgn_error_from_python();

	size_t len = PySequence_Fast_GET_SIZE(obj);
	if (len > 1 && (flags & DRGN_FIND_SYMBOL_ONE))  {
		return drgn_error_create(DRGN_ERROR_INVALID_ARGUMENT,
					 "symbol finder returned multiple elements, but one was requested");
	}

	for (size_t i = 0; i < len; i++) {
		PyObject *item = PySequence_Fast_GET_ITEM(obj, i);
		if (!PyObject_TypeCheck(item, &Symbol_type))
			return drgn_error_create(DRGN_ERROR_TYPE,
						 "symbol finder results must be of type Symbol");
		_cleanup_free_ struct drgn_symbol *sym = malloc(sizeof(*sym));
		if (!sym)
			return &drgn_enomem;
		struct drgn_error *err = drgn_symbol_copy(sym, ((Symbol *)item)->sym);
		if (err)
			return err;

		if (!drgn_symbol_result_builder_add(builder, sym))
			return &drgn_enomem;
		sym = NULL; // owned by the builder now
	}

	return NULL;
}

#define debug_info_finder_arg(self, fn) PyObject *arg = fn;
#define type_finder_arg(self, fn)						\
	_cleanup_pydecref_ PyObject *arg = Py_BuildValue("OO", self, fn);	\
	if (!arg)								\
		return NULL;
#define object_finder_arg(self, fn) PyObject *arg = fn;
#define symbol_finder_arg type_finder_arg

#define DEFINE_PROGRAM_FINDER_METHODS(which)					\
static PyObject *Program_register_##which##_finder(Program *self,		\
						   PyObject *args,		\
						   PyObject *kwds)		\
{										\
	struct drgn_error *err;							\
	static char *keywords[] = {"name", "fn", "enable_index", NULL};		\
	const char *name;							\
	PyObject *fn;								\
	PyObject *enable_index_obj = Py_None;					\
	if (!PyArg_ParseTupleAndKeywords(args, kwds,				\
					 "sO|$O:register_" #which "_finder",	\
					 keywords, &name, &fn,			\
					 &enable_index_obj))			\
		return NULL;							\
										\
	if (!PyCallable_Check(fn)) {						\
		PyErr_SetString(PyExc_TypeError, "fn must be callable");	\
		return NULL;							\
	}									\
										\
	size_t enable_index;							\
	if (enable_index_obj == Py_None) {					\
		enable_index = DRGN_HANDLER_REGISTER_DONT_ENABLE;		\
	} else {								\
		_cleanup_pydecref_ PyObject *negative_one = PyLong_FromLong(-1);\
		if (!negative_one)						\
			return NULL;						\
		int eq = PyObject_RichCompareBool(enable_index_obj,		\
						  negative_one, Py_EQ);		\
		if (eq < 0)							\
			return NULL;						\
		if (eq) {							\
			enable_index = DRGN_HANDLER_REGISTER_ENABLE_LAST;	\
		} else {							\
			enable_index = PyLong_AsSize_t(enable_index_obj);	\
			if (enable_index == (size_t)-1 && PyErr_Occurred())	\
				return NULL;					\
			/*							\
			 * If the index happens to be the			\
			 * DRGN_HANDLER_REGISTER_DONT_ENABLE sentinel		\
			 * (SIZE_MAX - 1), set it to something else; it's	\
			 * impossible to have this many finders anyways.	\
			 */							\
			if (enable_index == DRGN_HANDLER_REGISTER_DONT_ENABLE)	\
				enable_index--;					\
		}								\
	}									\
										\
	which##_finder_arg(self, fn)						\
	if (!Program_hold_reserve(self, 1))					\
		return NULL;							\
	const struct drgn_##which##_finder_ops ops = {				\
		.find = py_##which##_find_fn,					\
	};									\
	err = drgn_program_register_##which##_finder(&self->prog, name, &ops,	\
						     arg, enable_index);	\
	if (err)								\
		return set_drgn_error(err);					\
	Program_hold_object(self, arg);						\
	Py_RETURN_NONE;								\
										\
}										\
										\
static PyObject *Program_registered_##which##_finders(Program *self)		\
{										\
	struct drgn_error *err;							\
	_cleanup_free_ const char **names = NULL;				\
	size_t count;								\
	err = drgn_program_registered_##which##_finders(&self->prog, &names,	\
							&count);		\
	if (err)								\
		return set_drgn_error(err);					\
	_cleanup_pydecref_ PyObject *res = PySet_New(NULL);			\
	if (!res)								\
		return NULL;							\
	for (size_t i = 0; i < count; i++) {					\
		_cleanup_pydecref_ PyObject *name =				\
			PyUnicode_FromString(names[i]);				\
		if (!name)							\
			return NULL;						\
		if (PySet_Add(res, name))					\
			return NULL;						\
	}									\
	return_ptr(res);							\
}										\
										\
static PyObject *Program_set_enabled_##which##_finders(Program *self,		\
						       PyObject *args,		\
						       PyObject *kwds)		\
{										\
	struct drgn_error *err;							\
	static char *keywords[] = {"names", NULL};				\
	PyObject *names_obj;							\
	if (!PyArg_ParseTupleAndKeywords(args, kwds,				\
					 "O:set_enabled_" #which "_finders",	\
					 keywords, &names_obj))			\
		return NULL;							\
	_cleanup_pydecref_ PyObject *names_seq =				\
		PySequence_Fast(names_obj, "names must be sequence");		\
	if (!names_seq)								\
		return NULL;							\
	size_t count = PySequence_Fast_GET_SIZE(names_seq);			\
	_cleanup_free_ const char **names =					\
		malloc_array(count, sizeof(names[0]));				\
	if (!names)								\
		return NULL;							\
	for (size_t i = 0; i < count; i++) {					\
		names[i] = PyUnicode_AsUTF8(PySequence_Fast_GET_ITEM(names_seq, i));\
		if (!names[i])							\
			return NULL;						\
	}									\
	err = drgn_program_set_enabled_##which##_finders(&self->prog, names,	\
							 count);		\
	if (err)								\
		return set_drgn_error(err);					\
	Py_RETURN_NONE;								\
}										\
										\
static PyObject *Program_enabled_##which##_finders(Program *self)		\
{										\
	struct drgn_error *err;							\
	_cleanup_free_ const char **names = NULL;				\
	size_t count;								\
	err = drgn_program_enabled_##which##_finders(&self->prog, &names,	\
						     &count);			\
	if (err)								\
		return set_drgn_error(err);					\
	_cleanup_pydecref_ PyObject *res = PyList_New(count);			\
	if (!res)								\
		return NULL;							\
	for (size_t i = 0; i < count; i++) {					\
		PyObject *name = PyUnicode_FromString(names[i]);		\
		if (!name)							\
			return NULL;						\
		PyList_SET_ITEM(res, i, name);					\
	}									\
	return_ptr(res);							\
}

DEFINE_PROGRAM_FINDER_METHODS(debug_info)
DEFINE_PROGRAM_FINDER_METHODS(type)
DEFINE_PROGRAM_FINDER_METHODS(object)
DEFINE_PROGRAM_FINDER_METHODS(symbol)

static PyObject *deprecated_finder_name_obj(PyObject *fn)
{
	_cleanup_pydecref_ PyObject *name_attr_obj =
		PyObject_GetAttrString(fn, "__name__");
	if (name_attr_obj) {
		return PyUnicode_FromFormat("%S_%lu", name_attr_obj, random());
	} else if (PyErr_ExceptionMatches(PyExc_AttributeError)) {
		PyErr_Clear();
		return PyUnicode_FromFormat("%lu", random());
	} else {
		return NULL;
	}
}

static PyObject *Program_add_type_finder(Program *self, PyObject *args,
					 PyObject *kwds)
{
	struct drgn_error *err;
	static char *keywords[] = {"fn", NULL};
	PyObject *fn;
	if (!PyArg_ParseTupleAndKeywords(args, kwds, "O:add_type_finder",
					 keywords, &fn))
	    return NULL;

	if (!PyCallable_Check(fn)) {
		PyErr_SetString(PyExc_TypeError, "fn must be callable");
		return NULL;
	}

	_cleanup_pydecref_ PyObject *arg = Py_BuildValue("OO", self, fn);
	if (!arg)
		return NULL;

	_cleanup_pydecref_ PyObject *name_obj = deprecated_finder_name_obj(fn);
	if (!name_obj)
		return NULL;
	const char *name = PyUnicode_AsUTF8(name_obj);
	if (!name)
		return NULL;

	if (!Program_hold_reserve(self, 1))
		return NULL;
	const struct drgn_type_finder_ops ops = {
		.find = py_type_find_fn_old,
	};
	err = drgn_program_register_type_finder(&self->prog, name, &ops, arg,
						0);
	if (err)
		return set_drgn_error(err);
	Program_hold_object(self, arg);
	Py_RETURN_NONE;
}

static PyObject *Program_add_object_finder(Program *self, PyObject *args,
					   PyObject *kwds)
{
	struct drgn_error *err;

	static char *keywords[] = {"fn", NULL};
	PyObject *fn;
	if (!PyArg_ParseTupleAndKeywords(args, kwds, "O:add_object_finder",
					 keywords, &fn))
	    return NULL;

	if (!PyCallable_Check(fn)) {
		PyErr_SetString(PyExc_TypeError, "fn must be callable");
		return NULL;
	}

	_cleanup_pydecref_ PyObject *name_obj = deprecated_finder_name_obj(fn);
	if (!name_obj)
		return NULL;
	const char *name = PyUnicode_AsUTF8(name_obj);
	if (!name)
		return NULL;

	if (!Program_hold_reserve(self, 1))
		return NULL;
	const struct drgn_object_finder_ops ops = {
		.find = py_object_find_fn,
	};
	err = drgn_program_register_object_finder(&self->prog, name, &ops, fn,
						  0);
	if (err)
		return set_drgn_error(err);
	Program_hold_object(self, fn);
	Py_RETURN_NONE;
}

static PyObject *Program_set_core_dump(Program *self, PyObject *args,
				       PyObject *kwds)
{
	static char *keywords[] = {"path", NULL};
	struct drgn_error *err;
	PATH_ARG(path, .allow_fd = true);

	if (!PyArg_ParseTupleAndKeywords(args, kwds, "O&:set_core_dump",
					 keywords, path_converter, &path))
		return NULL;

	if (path.fd >= 0)
		err = drgn_program_set_core_dump_fd(&self->prog, path.fd);
	else
		err = drgn_program_set_core_dump(&self->prog, path.path);
	if (err)
		return set_drgn_error(err);
	Py_RETURN_NONE;
}

static PyObject *Program_set_kernel(Program *self)
{
	struct drgn_error *err;

	err = drgn_program_set_kernel(&self->prog);
	if (err)
		return set_drgn_error(err);
	Py_RETURN_NONE;
}

static PyObject *Program_set_pid(Program *self, PyObject *args, PyObject *kwds)
{
	static char *keywords[] = {"pid", NULL};
	struct drgn_error *err;
	int pid;

	if (!PyArg_ParseTupleAndKeywords(args, kwds, "i:set_pid", keywords,
					 &pid))
		return NULL;

	err = drgn_program_set_pid(&self->prog, pid);
	if (err)
		return set_drgn_error(err);
	Py_RETURN_NONE;
}

static ModuleIterator *Program_modules(Program *self)
{
	struct drgn_error *err;
	ModuleIterator *it = call_tp_alloc(ModuleIterator);
	if (!it)
		return NULL;
	err = drgn_created_module_iterator_create(&self->prog, &it->it);
	if (err) {
		it->it = NULL;
		Py_DECREF(it);
		return set_drgn_error(err);
	}
	Py_INCREF(self);
	return it;
}

static ModuleIterator *Program_loaded_modules(Program *self)
{
	struct drgn_error *err;
	ModuleIterator *it =
		(ModuleIterator *)ModuleIteratorWithNew_type.tp_alloc(
					&ModuleIteratorWithNew_type, 0);
	if (!it)
		return NULL;
	err = drgn_loaded_module_iterator_create(&self->prog, &it->it);
	if (err) {
		it->it = NULL;
		Py_DECREF(it);
		return set_drgn_error(err);
	}
	Py_INCREF(self);
	return it;
}

static PyObject *Program_create_loaded_modules(Program *self)
{
	struct drgn_error *err = drgn_create_loaded_modules(&self->prog);
	if (err)
		return set_drgn_error(err);
	Py_RETURN_NONE;
}

static inline PyObject *Module_wrap_find(struct drgn_module *module)
{
	if (module)
		return Module_wrap(module);
	PyErr_SetString(PyExc_LookupError, "module not found");
	return NULL;
}

static PyObject *Program_main_module(Program *self, PyObject *args,
				     PyObject *kwds)
{
	struct drgn_error *err;
	static char *keywords[] = {"name", "create", NULL};
	PATH_ARG(name);
	int create = 0;
	if (!PyArg_ParseTupleAndKeywords(args, kwds, "|O&$p:main_module",
					 keywords, path_converter, &name,
					 &create))
		return NULL;

	if (create) {
		if (!name.path) {
			PyErr_SetString(PyExc_TypeError,
					"name must be given if create=True");
			return NULL;
		}
		struct drgn_module *module;
		err = drgn_module_find_or_create_main(&self->prog, name.path,
						      &module, NULL);
		if (err) {
			set_drgn_error(err);
			return NULL;
		}
		return Module_wrap(module);
	} else {
		return Module_wrap_find(drgn_module_find_main(&self->prog,
							      name.path));
	}
}

static PyObject *Program_shared_library_module(Program *self, PyObject *args,
					       PyObject *kwds)
{
	struct drgn_error *err;
	static char *keywords[] = {"name", "dynamic_address", "create", NULL};
	PATH_ARG(name);
	struct index_arg dynamic_address = {};
	int create = 0;
	if (!PyArg_ParseTupleAndKeywords(args, kwds,
					 "O&O&|$p:shared_library_module",
					 keywords, path_converter, &name,
					 index_converter, &dynamic_address,
					 &create))
		return NULL;

	if (create) {
		struct drgn_module *module;
		err = drgn_module_find_or_create_shared_library(&self->prog,
								name.path,
								dynamic_address.uvalue,
								&module, NULL);
		if (err) {
			set_drgn_error(err);
			return NULL;
		}
		return Module_wrap(module);
	} else {
		return Module_wrap_find(drgn_module_find_shared_library(&self->prog,
									name.path,
									dynamic_address.uvalue));
	}
}

static PyObject *Program_vdso_module(Program *self, PyObject *args,
				     PyObject *kwds)
{
	struct drgn_error *err;
	static char *keywords[] = {"name", "dynamic_address", "create", NULL};
	PATH_ARG(name);
	struct index_arg dynamic_address = {};
	int create = 0;
	if (!PyArg_ParseTupleAndKeywords(args, kwds, "O&O&|$p:vdso_module",
					 keywords, path_converter, &name,
					 index_converter, &dynamic_address,
					 &create))
		return NULL;

	if (create) {
		struct drgn_module *module;
		err = drgn_module_find_or_create_vdso(&self->prog, name.path,
						      dynamic_address.uvalue,
						      &module, NULL);
		if (err) {
			set_drgn_error(err);
			return NULL;
		}
		return Module_wrap(module);
	} else {
		return Module_wrap_find(drgn_module_find_vdso(&self->prog,
							      name.path,
							      dynamic_address.uvalue));
	}
}

static PyObject *Program_relocatable_module(Program *self, PyObject *args,
					    PyObject *kwds)
{
	struct drgn_error *err;
	static char *keywords[] = {"name", "address", "create", NULL};
	PATH_ARG(name);
	struct index_arg address = {};
	int create = 0;
	if (!PyArg_ParseTupleAndKeywords(args, kwds,
					 "O&O&|$p:relocatable_module", keywords,
					 path_converter, &name, index_converter,
					 &address, &create))
		return NULL;

	if (create) {
		struct drgn_module *module;
		err = drgn_module_find_or_create_relocatable(&self->prog,
							     name.path,
							     address.uvalue,
							     &module, NULL);
		if (err) {
			set_drgn_error(err);
			return NULL;
		}
		return Module_wrap(module);
	} else {
		return Module_wrap_find(drgn_module_find_relocatable(&self->prog,
								     name.path,
								     address.uvalue));
	}
}

static PyObject *Program_linux_kernel_loadable_module(Program *self,
						      PyObject *args,
						      PyObject *kwds)
{
	struct drgn_error *err;
	static char *keywords[] = {"module_obj", "create", NULL};
	DrgnObject *module_obj;
	int create = 0;
	if (!PyArg_ParseTupleAndKeywords(args, kwds,
					 "O!|$p:linux_kernel_loadable_module",
					 keywords, &DrgnObject_type,
					 &module_obj, &create))
		return NULL;

	if (DrgnObject_prog(module_obj) != self) {
		PyErr_SetString(PyExc_ValueError,
				"object is from different program");
		return NULL;
	}

	struct drgn_module *module;
	if (create) {
		err = drgn_module_find_or_create_linux_kernel_loadable(&module_obj->obj,
								       &module,
								       NULL);
		if (err) {
			set_drgn_error(err);
			return NULL;
		}
		return Module_wrap(module);
	} else {
		err = drgn_module_find_linux_kernel_loadable(&module_obj->obj,
							     &module);
		if (err) {
			set_drgn_error(err);
			return NULL;
		}
		return Module_wrap_find(module);
	}
}

static PyObject *Program_extra_module(Program *self, PyObject *args,
				      PyObject *kwds)
{
	struct drgn_error *err;
	static char *keywords[] = {"name", "id", "create", NULL};
	PATH_ARG(name);
	struct index_arg id = {};
	int create = 0;
	if (!PyArg_ParseTupleAndKeywords(args, kwds, "O&|O&$p:extra_module",
					 keywords, path_converter, &name,
					 index_converter, &id, &create))
		return NULL;

	if (create) {
		struct drgn_module *module;
		err = drgn_module_find_or_create_extra(&self->prog, name.path,
						       id.uvalue, &module,
						       NULL);
		if (err) {
			set_drgn_error(err);
			return NULL;
		}
		return Module_wrap(module);
	} else {
		return Module_wrap_find(drgn_module_find_extra(&self->prog,
							       name.path,
							       id.uvalue));
	}
}

static PyObject *Program_module(Program *self, PyObject *arg)
{
	struct drgn_module *module;
	if (PyUnicode_Check(arg)) {
		const char *name = PyUnicode_AsUTF8(arg);
		if (!name)
			return NULL;
		module = drgn_module_find_by_name(&self->prog, name);
	} else {
		struct index_arg address = {};
		if (!index_converter(arg, &address))
			return NULL;
		module = drgn_module_find_by_address(&self->prog,
						     address.uvalue);
	}
	return Module_wrap_find(module);
}

static DebugInfoOptions *Program_get_debug_info_options(Program *self, void *arg)
{
	DebugInfoOptions *options = call_tp_alloc(DebugInfoOptions);
	if (options) {
		options->options = drgn_program_debug_info_options(&self->prog);
		options->prog = self;
		Py_INCREF(self);
	}
	return options;
}

static int Program_set_debug_info_options(Program *self, PyObject *value, void *arg)
{
	SETTER_NO_DELETE("debug_info_options", value);
	if (!PyObject_TypeCheck(value, &DebugInfoOptions_type)) {
		PyErr_SetString(PyExc_TypeError,
				"debug_info_options must be DebugInfoOptions");
		return -1;
	}
	struct drgn_error *err =
		drgn_debug_info_options_copy(drgn_program_debug_info_options(&self->prog),
					     ((DebugInfoOptions *)value)->options);
	if (err) {
		set_drgn_error(err);
		return -1;
	}
	return 0;
}

static PyObject *Program_load_debug_info(Program *self, PyObject *args,
					 PyObject *kwds)
{
	static char *keywords[] = {"paths", "default", "main", NULL};
	struct drgn_error *err;
	PATH_SEQUENCE_ARG(paths, .allow_none = true);
	int load_default = 0;
	int load_main = 0;
	if (!PyArg_ParseTupleAndKeywords(args, kwds, "|O&pp:load_debug_info",
					 keywords, path_sequence_converter,
					 &paths, &load_default, &load_main))
		return NULL;
	err = drgn_program_load_debug_info(&self->prog, paths.paths,
					   path_sequence_size(&paths),
					   load_default, load_main);
	if (err) {
		set_drgn_error(err);
		return NULL;
	}
	Py_RETURN_NONE;
}

static PyObject *Program_load_default_debug_info(Program *self)
{
	struct drgn_error *err;

	err = drgn_program_load_debug_info(&self->prog, NULL, 0, true, true);
	if (err)
		return set_drgn_error(err);
	Py_RETURN_NONE;
}

static PyObject *Program_load_module_debug_info(Program *self, PyObject *args)
{
	size_t num_modules = PyTuple_GET_SIZE(args);
	_cleanup_free_ struct drgn_module **modules =
		malloc_array(num_modules, sizeof(*modules));
	if (!modules) {
		PyErr_NoMemory();
		return NULL;
	}

	for (size_t i = 0; i < num_modules; i++) {
		PyObject *item = PyTuple_GET_ITEM(args, i);
		if (!PyObject_TypeCheck(item, &Module_type)) {
			return PyErr_Format(PyExc_TypeError,
					    "expected Module, not %s",
					    Py_TYPE(item)->tp_name);
		}
		modules[i] = ((Module *)item)->module;
		if (modules[i]->prog != &self->prog) {
			PyErr_SetString(PyExc_ValueError,
					"module from wrong program");
			return NULL;
		}
	}

	struct drgn_error *err =
		drgn_load_module_debug_info(modules, &num_modules);
	if (err)
		return set_drgn_error(err);
	Py_RETURN_NONE;
}

DEFINE_VECTOR(drgn_module_vector, struct drgn_module *);

static PyObject *Program_find_standard_debug_info(Program *self, PyObject *args,
						  PyObject *kwds)
{
	struct drgn_error *err;
	static char *keywords[] = {"modules", "options", NULL};
	PyObject *modules_obj;
	PyObject *options_obj = Py_None;
	if (!PyArg_ParseTupleAndKeywords(args, kwds,
					 "O|O:find_standard_debug_info",
					 keywords, &modules_obj, &options_obj))
	    return NULL;

	_cleanup_pydecref_ PyObject *it = PyObject_GetIter(modules_obj);
	if (!it)
		return NULL;

	Py_ssize_t length_hint = PyObject_LengthHint(modules_obj, 1);
	if (length_hint == -1)
		return 0;

	VECTOR(drgn_module_vector, modules);
	if (!drgn_module_vector_reserve(&modules, length_hint))
		return PyErr_NoMemory();

	for (;;) {
		_cleanup_pydecref_ PyObject *item = PyIter_Next(it);
		if (!item)
			break;

		if (!PyObject_TypeCheck(item, &Module_type)) {
			return PyErr_Format(PyExc_TypeError,
					    "expected Module, not %s",
					    Py_TYPE(item)->tp_name);
		}
		struct drgn_module *module = ((Module *)item)->module;
		if (module->prog != &self->prog) {
			PyErr_SetString(PyExc_ValueError,
					"module from wrong program");
			return NULL;
		}
		if (!drgn_module_vector_append(&modules, &module))
			return PyErr_NoMemory();
	}
	if (PyErr_Occurred())
		return NULL;

	struct drgn_debug_info_options *options;
	if (options_obj == Py_None) {
		options = NULL;
	} else if (PyObject_TypeCheck(options_obj, &DebugInfoOptions_type)) {
		options = ((DebugInfoOptions *)options_obj)->options;
	} else {
		PyErr_SetString(PyExc_TypeError,
				"options must be DebugInfoOptions or None");
		return NULL;
	}

	err = drgn_find_standard_debug_info(drgn_module_vector_begin(&modules),
					    drgn_module_vector_size(&modules),
					    options);
	if (err)
		return set_drgn_error(err);
	Py_RETURN_NONE;
}

static PyObject *Program_read(Program *self, PyObject *args, PyObject *kwds)
{
	static char *keywords[] = {"address", "size", "physical", NULL};
	struct drgn_error *err;
	struct index_arg address = {};
	Py_ssize_t size;
	int physical = 0;
	bool clear;
	if (!PyArg_ParseTupleAndKeywords(args, kwds, "O&n|p:read", keywords,
					 index_converter, &address, &size,
					 &physical))
	    return NULL;

	if (size < 0) {
		PyErr_SetString(PyExc_ValueError, "negative size");
		return NULL;
	}
	_cleanup_pydecref_ PyObject *buf =
		PyBytes_FromStringAndSize(NULL, size);
	if (!buf)
		return NULL;
	clear = set_drgn_in_python();
	err = drgn_program_read_memory(&self->prog, PyBytes_AS_STRING(buf),
				       address.uvalue, size, physical);
	if (clear)
		clear_drgn_in_python();
	if (err)
		return set_drgn_error(err);
	return_ptr(buf);
}

#define METHOD_READ(x, type)							\
static PyObject *Program_read_##x(Program *self, PyObject *args,		\
				  PyObject *kwds)				\
{										\
	static char *keywords[] = {"address", "physical", NULL};		\
	struct drgn_error *err;							\
	struct index_arg address = {};						\
	int physical = 0;							\
	type tmp;								\
										\
	if (!PyArg_ParseTupleAndKeywords(args, kwds, "O&|p:read_"#x, keywords,	\
					 index_converter, &address, &physical))	\
	    return NULL;							\
										\
	err = drgn_program_read_##x(&self->prog, address.uvalue, physical,	\
				    &tmp);					\
	if (err)								\
		return set_drgn_error(err);					\
	if (sizeof(tmp) <= sizeof(unsigned long))				\
		return PyLong_FromUnsignedLong(tmp);				\
	else									\
		return PyLong_FromUnsignedLongLong(tmp);			\
}
METHOD_READ(u8, uint8_t)
METHOD_READ(u16, uint16_t)
METHOD_READ(u32, uint32_t)
METHOD_READ(u64, uint64_t)
METHOD_READ(word, uint64_t)
#undef METHOD_READ

static PyObject *Program_find_type(Program *self, PyObject *args, PyObject *kwds)
{
	static char *keywords[] = {"name", "filename", NULL};
	struct drgn_error *err;
	PyObject *name_or_type;
	PATH_ARG(filename, .allow_none = true);
	if (!PyArg_ParseTupleAndKeywords(args, kwds, "O|O&:type", keywords,
					 &name_or_type, path_converter,
					 &filename))
		return NULL;

	if (PyObject_TypeCheck(name_or_type, &DrgnType_type)) {
		if (DrgnType_prog((DrgnType *)name_or_type) != self) {
			PyErr_SetString(PyExc_ValueError,
					"type is from different program");
			return NULL;
		}
		Py_INCREF(name_or_type);
		return name_or_type;
	} else if (!PyUnicode_Check(name_or_type)) {
		PyErr_SetString(PyExc_TypeError,
				"type() argument 1 must be str or Type");
		return NULL;
	}

	const char *name = PyUnicode_AsUTF8(name_or_type);
	if (!name)
		return NULL;
	bool clear = set_drgn_in_python();
	struct drgn_qualified_type qualified_type;
	err = drgn_program_find_type(&self->prog, name, filename.path,
				     &qualified_type);
	if (clear)
		clear_drgn_in_python();
	if (err) {
		set_drgn_error(err);
		return NULL;
	}
	return DrgnType_wrap(qualified_type);
}

static void *set_object_not_found_error(struct drgn_error *err, PyObject *name)
{
	_cleanup_pydecref_ PyObject *args = Py_BuildValue("(s)", err->message);
	drgn_error_destroy(err);
	if (!args)
		return NULL;

	_cleanup_pydecref_ PyObject *kwargs =
		Py_BuildValue("{sO}", "name", name);
	if (!kwargs)
		return NULL;

	_cleanup_pydecref_ PyObject *exc =
		PyObject_Call((PyObject *)&ObjectNotFoundError_type, args,
			      kwargs);
	if (exc)
		PyErr_SetObject((PyObject *)&ObjectNotFoundError_type, exc);
	return NULL;
}

static DrgnObject *Program_find_object(Program *self, PyObject *name_obj,
				       const char *filename,
				       enum drgn_find_object_flags flags)
{
	struct drgn_error *err;

	if (!PyUnicode_Check(name_obj)) {
		PyErr_Format(PyExc_TypeError, "name must be str, not %.200s",
			     Py_TYPE(name_obj)->tp_name);
		return NULL;
	}
	const char *name = PyUnicode_AsUTF8(name_obj);
	if (!name)
		return NULL;

	_cleanup_pydecref_ DrgnObject *ret = DrgnObject_alloc(self);
	if (!ret)
		return NULL;
	bool clear = set_drgn_in_python();
	err = drgn_program_find_object(&self->prog, name, filename, flags,
				       &ret->obj);
	if (clear)
		clear_drgn_in_python();
	if (err && err->code == DRGN_ERROR_LOOKUP)
		return set_object_not_found_error(err, name_obj);
	else if (err)
		return set_drgn_error(err);
	return_ptr(ret);
}

static DrgnObject *Program_object(Program *self, PyObject *args,
				  PyObject *kwds)
{
	static char *keywords[] = {"name", "flags", "filename", NULL};
	PyObject *name;
	struct enum_arg flags = {
		.type = FindObjectFlags_class,
		.value = DRGN_FIND_OBJECT_ANY,
	};
	PATH_ARG(filename, .allow_none = true);
	if (!PyArg_ParseTupleAndKeywords(args, kwds, "O|O&O&:object", keywords,
					 &name, enum_converter, &flags,
					 path_converter, &filename))
		return NULL;

	return Program_find_object(self, name, filename.path, flags.value);
}

static DrgnObject *Program_constant(Program *self, PyObject *args,
				    PyObject *kwds)
{
	static char *keywords[] = {"name", "filename", NULL};
	PyObject *name;
	PATH_ARG(filename, .allow_none = true);
	if (!PyArg_ParseTupleAndKeywords(args, kwds, "O|O&:constant", keywords,
					 &name, path_converter, &filename))
		return NULL;

	return Program_find_object(self, name, filename.path,
				   DRGN_FIND_OBJECT_CONSTANT);
}

static DrgnObject *Program_function(Program *self, PyObject *args,
				    PyObject *kwds)
{
	static char *keywords[] = {"name", "filename", NULL};
	PyObject *name;
	PATH_ARG(filename, .allow_none = true);
	if (!PyArg_ParseTupleAndKeywords(args, kwds, "O|O&:function", keywords,
					 &name, path_converter, &filename))
		return NULL;

	return Program_find_object(self, name, filename.path,
				   DRGN_FIND_OBJECT_FUNCTION);
}

static DrgnObject *Program_variable(Program *self, PyObject *args,
				    PyObject *kwds)
{
	static char *keywords[] = {"name", "filename", NULL};
	PyObject *name;
	PATH_ARG(filename, .allow_none = true);
	if (!PyArg_ParseTupleAndKeywords(args, kwds, "O|O&:variable", keywords,
					 &name, path_converter, &filename))
		return NULL;

	return Program_find_object(self, name, filename.path,
				   DRGN_FIND_OBJECT_VARIABLE);
}

static PyObject *Program_stack_trace(Program *self, PyObject *args,
				     PyObject *kwds)
{
	static char *keywords[] = {"thread", NULL};
	struct drgn_error *err;
	PyObject *thread;
	struct drgn_stack_trace *trace;

	if (!PyArg_ParseTupleAndKeywords(args, kwds, "O:stack_trace", keywords,
					 &thread))
		return NULL;

	if (PyObject_TypeCheck(thread, &DrgnObject_type)) {
		err = drgn_object_stack_trace(&((DrgnObject *)thread)->obj,
					      &trace);
	} else {
		struct index_arg tid = {};

		if (!index_converter(thread, &tid))
			return NULL;
		err = drgn_program_stack_trace(&self->prog, tid.uvalue, &trace);
	}
	if (err)
		return set_drgn_error(err);

	PyObject *ret = StackTrace_wrap(trace);
	if (!ret)
		drgn_stack_trace_destroy(trace);
	return ret;
}

static PyObject *Program_stack_trace_from_pcs(Program *self, PyObject *args,
					      PyObject *kwds)
{
	static char *keywords[] = {"pcs", NULL};
	struct drgn_error *err;
	PyObject *pypcs;
	struct drgn_stack_trace *trace;

	if (!PyArg_ParseTupleAndKeywords(args, kwds, "O:stack_trace_from_pcs",
					 keywords, &pypcs))
		return NULL;

	_cleanup_pydecref_ PyObject *pypcseq = PySequence_Fast(
		pypcs, "stack_trace_from_pcs() argument 1 must be a list");
	if (!pypcseq)
		return NULL;

	size_t size = PySequence_Fast_GET_SIZE(pypcseq);
	_cleanup_free_ uint64_t *pcs = malloc_array(size, sizeof(uint64_t));
	for (uint64_t i = 0; i != size; ++i) {
		struct index_arg pc = {};

		if (!index_converter(PySequence_Fast_GET_ITEM(pypcseq, i), &pc))
			return NULL;
		pcs[i] = pc.uvalue;
	}

	err = drgn_program_stack_trace_from_pcs(&self->prog, pcs, size, &trace);
	if (err)
		return set_drgn_error(err);

	PyObject *ret = StackTrace_wrap(trace);
	if (!ret)
		drgn_stack_trace_destroy(trace);
	return ret;
}

static PyObject *Program_symbols(Program *self, PyObject *args)
{
	struct drgn_error *err;

	PyObject *arg = Py_None;
	if (!PyArg_ParseTuple(args, "|O:symbols", &arg))
		return NULL;

	struct drgn_symbol **symbols;
	size_t count;
	if (arg == Py_None) {
		err = drgn_program_find_symbols_by_name(&self->prog, NULL,
							&symbols, &count);
	} else if (PyUnicode_Check(arg)) {
		const char *name = PyUnicode_AsUTF8(arg);
		if (!name)
			return NULL;
		err = drgn_program_find_symbols_by_name(&self->prog, name,
							&symbols, &count);
	} else {
		struct index_arg address = {};
		if (!index_converter(arg, &address))
			return NULL;
		err = drgn_program_find_symbols_by_address(&self->prog,
							   address.uvalue,
							   &symbols, &count);
	}
	if (err)
		return set_drgn_error(err);

	return Symbol_list_wrap(symbols, count, (PyObject *)self);
}

static PyObject *Program_symbol(Program *self, PyObject *arg)
{
	struct drgn_error *err;
	struct drgn_symbol *sym;
	PyObject *ret;

	if (PyUnicode_Check(arg)) {
		const char *name;

		name = PyUnicode_AsUTF8(arg);
		if (!name)
			return NULL;
		err = drgn_program_find_symbol_by_name(&self->prog, name, &sym);
	} else {
		struct index_arg address = {};

		if (!index_converter(arg, &address))
			return NULL;
		err = drgn_program_find_symbol_by_address(&self->prog,
							  address.uvalue, &sym);
	}
	if (err)
		return set_drgn_error(err);
	ret = Symbol_wrap(sym, (PyObject *)self);
	if (!ret) {
		drgn_symbol_destroy(sym);
		return NULL;
	}
	return ret;
}

static ThreadIterator *Program_threads(Program *self)
{
	struct drgn_thread_iterator *it;
	struct drgn_error *err = drgn_thread_iterator_create(&self->prog, &it);
	if (err)
		return set_drgn_error(err);
	ThreadIterator *ret = call_tp_alloc(ThreadIterator);
	if (!ret) {
		drgn_thread_iterator_destroy(it);
		return NULL;
	}
	ret->prog = self;
	ret->iterator = it;
	Py_INCREF(self);
	return ret;
}

static PyObject *Program_thread(Program *self, PyObject *args, PyObject *kwds)
{
	static char *keywords[] = {"tid", NULL};
	struct drgn_error *err;
	struct index_arg tid = {};

	if (!PyArg_ParseTupleAndKeywords(args, kwds, "O&:thread", keywords,
					 index_converter, &tid))
		return NULL;

	struct drgn_thread *thread;
	err = drgn_program_find_thread(&self->prog, tid.uvalue, &thread);
	if (err)
		return set_drgn_error(err);
	if (!thread) {
		return PyErr_Format(PyExc_LookupError,
				    "thread with ID %llu not found",
				    tid.uvalue);
	}
	PyObject *ret = Thread_wrap(thread);
	drgn_thread_destroy(thread);
	return ret;
}

static PyObject *Program_main_thread(Program *self)
{
	struct drgn_error *err;
	struct drgn_thread *thread;
	err = drgn_program_main_thread(&self->prog, &thread);
	if (err)
		return set_drgn_error(err);
	return Thread_wrap(thread);
}

static PyObject *Program_crashed_thread(Program *self)
{
	struct drgn_error *err;
	struct drgn_thread *thread;
	err = drgn_program_crashed_thread(&self->prog, &thread);
	if (err)
		return set_drgn_error(err);
	return Thread_wrap(thread);
}

// Used for testing.
static PyObject *Program__log(Program *self, PyObject *args, PyObject *kwds)
{
	int level;
	const char *str;
	if (!PyArg_ParseTuple(args, "is", &level, &str))
		return NULL;
	drgn_log(level, &self->prog, "%s", str);
	Py_RETURN_NONE;
}

static DrgnObject *Program_subscript(Program *self, PyObject *key)
{
	if (!PyUnicode_Check(key)) {
		PyErr_SetObject(PyExc_KeyError, key);
		return NULL;
	}
	return Program_find_object(self, key, NULL, DRGN_FIND_OBJECT_ANY);
}

static int Program_contains(Program *self, PyObject *key)
{
	struct drgn_error *err;

	if (!PyUnicode_Check(key)) {
		PyErr_SetObject(PyExc_KeyError, key);
		return 0;
	}

	const char *name = PyUnicode_AsUTF8(key);
	if (!name)
		return -1;

	DRGN_OBJECT(tmp, &self->prog);
	bool clear = set_drgn_in_python();
	err = drgn_program_find_object(&self->prog, name, NULL,
				       DRGN_FIND_OBJECT_ANY, &tmp);
	if (clear)
		clear_drgn_in_python();
	if (err) {
		if (err->code == DRGN_ERROR_LOOKUP) {
			drgn_error_destroy(err);
			return 0;
		} else {
			set_drgn_error(err);
			return -1;
		}
	}
	return 1;
}

static PyObject *Program_get_flags(Program *self, void *arg)
{
	return PyObject_CallFunction(ProgramFlags_class, "k",
				     (unsigned long)self->prog.flags);
}

static PyObject *Program_get_platform(Program *self, void *arg)
{
	const struct drgn_platform *platform;

	platform = drgn_program_platform(&self->prog);
	if (platform)
		return Platform_wrap(platform);
	else
		Py_RETURN_NONE;
}

static PyObject *Program_get_core_dump_path(Program *self, void *arg)
{
	const char *path = drgn_program_core_dump_path(&self->prog);
	if (!path)
		Py_RETURN_NONE;
	return PyUnicode_FromString(path);
}

static PyObject *Program_get_language(Program *self, void *arg)
{
	return Language_wrap(drgn_program_language(&self->prog));
}

static int Program_set_language(Program *self, PyObject *value, void *arg)
{
	SETTER_NO_DELETE("language", value);
	if (!PyObject_TypeCheck(value, &Language_type)) {
		PyErr_SetString(PyExc_TypeError, "language must be Language");
		return -1;
	}
	drgn_program_set_language(&self->prog, ((Language *)value)->language);
	return 0;
}

#define PROGRAM_FINDER_METHOD_DEFS(which)					\
	{"register_" #which "_finder",						\
	 (PyCFunction)Program_register_##which##_finder,			\
	 METH_VARARGS | METH_KEYWORDS,						\
	 drgn_Program_register_##which##_finder_DOC},				\
	{"registered_" #which "_finders",					\
	 (PyCFunction)Program_registered_##which##_finders, METH_NOARGS,	\
	 drgn_Program_registered_##which##_finders_DOC},			\
	{"set_enabled_" #which "_finders",					\
	 (PyCFunction)Program_set_enabled_##which##_finders,			\
	 METH_VARARGS | METH_KEYWORDS,						\
	 drgn_Program_set_enabled_##which##_finders_DOC},			\
	{"enabled_" #which "_finders",						\
	 (PyCFunction)Program_enabled_##which##_finders, METH_NOARGS,		\
	 drgn_Program_enabled_##which##_finders_DOC}

static PyMethodDef Program_methods[] = {
	{"add_memory_segment", (PyCFunction)Program_add_memory_segment,
	 METH_VARARGS | METH_KEYWORDS, drgn_Program_add_memory_segment_DOC},
	PROGRAM_FINDER_METHOD_DEFS(debug_info),
	PROGRAM_FINDER_METHOD_DEFS(type),
	PROGRAM_FINDER_METHOD_DEFS(object),
	PROGRAM_FINDER_METHOD_DEFS(symbol),
	{"add_type_finder", (PyCFunction)Program_add_type_finder,
	 METH_VARARGS | METH_KEYWORDS, drgn_Program_add_type_finder_DOC},
	{"add_object_finder", (PyCFunction)Program_add_object_finder,
	 METH_VARARGS | METH_KEYWORDS, drgn_Program_add_object_finder_DOC},
	{"set_core_dump", (PyCFunction)Program_set_core_dump,
	 METH_VARARGS | METH_KEYWORDS, drgn_Program_set_core_dump_DOC},
	{"set_kernel", (PyCFunction)Program_set_kernel, METH_NOARGS,
	 drgn_Program_set_kernel_DOC},
	{"set_pid", (PyCFunction)Program_set_pid, METH_VARARGS | METH_KEYWORDS,
	 drgn_Program_set_pid_DOC},
	{"modules", (PyCFunction)Program_modules, METH_NOARGS,
	 drgn_Program_modules_DOC},
	{"loaded_modules", (PyCFunction)Program_loaded_modules, METH_NOARGS,
	 drgn_Program_loaded_modules_DOC},
	{"create_loaded_modules", (PyCFunction)Program_create_loaded_modules,
	 METH_NOARGS, drgn_Program_create_loaded_modules_DOC},
	{"main_module", (PyCFunction)Program_main_module,
	 METH_VARARGS | METH_KEYWORDS, drgn_Program_main_module_DOC},
	{"shared_library_module", (PyCFunction)Program_shared_library_module,
	 METH_VARARGS | METH_KEYWORDS, drgn_Program_shared_library_module_DOC},
	{"vdso_module", (PyCFunction)Program_vdso_module,
	 METH_VARARGS | METH_KEYWORDS, drgn_Program_vdso_module_DOC},
	{"relocatable_module", (PyCFunction)Program_relocatable_module,
	 METH_VARARGS | METH_KEYWORDS, drgn_Program_relocatable_module_DOC},
	{"linux_kernel_loadable_module",
	 (PyCFunction)Program_linux_kernel_loadable_module,
	 METH_VARARGS | METH_KEYWORDS,
	 drgn_Program_linux_kernel_loadable_module_DOC},
	{"extra_module", (PyCFunction)Program_extra_module,
	 METH_VARARGS | METH_KEYWORDS, drgn_Program_extra_module_DOC},
	{"module", (PyCFunction)Program_module, METH_O,
	 drgn_Program_module_DOC},
	{"load_debug_info", (PyCFunction)Program_load_debug_info,
	 METH_VARARGS | METH_KEYWORDS, drgn_Program_load_debug_info_DOC},
	{"load_default_debug_info",
	 (PyCFunction)Program_load_default_debug_info, METH_NOARGS,
	 drgn_Program_load_default_debug_info_DOC},
	{"load_module_debug_info", (PyCFunction)Program_load_module_debug_info,
	 METH_VARARGS, drgn_Program_load_module_debug_info_DOC},
	{"find_standard_debug_info",
	 (PyCFunction)Program_find_standard_debug_info,
	 METH_VARARGS | METH_KEYWORDS,
	 drgn_Program_find_standard_debug_info_DOC},
	{"__getitem__", (PyCFunction)Program_subscript, METH_O | METH_COEXIST,
	 drgn_Program___getitem___DOC},
	{"__contains__", (PyCFunction)Program_contains, METH_O | METH_COEXIST,
	 drgn_Program___contains___DOC},
	{"read", (PyCFunction)Program_read, METH_VARARGS | METH_KEYWORDS,
	 drgn_Program_read_DOC},
#define METHOD_DEF_READ(x)						\
	{"read_"#x, (PyCFunction)Program_read_##x,			\
	 METH_VARARGS | METH_KEYWORDS, drgn_Program_read_##x##_DOC}
	METHOD_DEF_READ(u8),
	METHOD_DEF_READ(u16),
	METHOD_DEF_READ(u32),
	METHOD_DEF_READ(u64),
	METHOD_DEF_READ(word),
#undef METHOD_READ_U
	{"type", (PyCFunction)Program_find_type, METH_VARARGS | METH_KEYWORDS,
	 drgn_Program_type_DOC},
	{"object", (PyCFunction)Program_object, METH_VARARGS | METH_KEYWORDS,
	 drgn_Program_object_DOC},
	{"constant", (PyCFunction)Program_constant,
	 METH_VARARGS | METH_KEYWORDS, drgn_Program_constant_DOC},
	{"function", (PyCFunction)Program_function,
	 METH_VARARGS | METH_KEYWORDS, drgn_Program_function_DOC},
	{"variable", (PyCFunction)Program_variable,
	 METH_VARARGS | METH_KEYWORDS, drgn_Program_variable_DOC},
	{"stack_trace", (PyCFunction)Program_stack_trace,
	 METH_VARARGS | METH_KEYWORDS, drgn_Program_stack_trace_DOC},
	{"stack_trace_from_pcs", (PyCFunction)Program_stack_trace_from_pcs,
	 METH_VARARGS | METH_KEYWORDS, drgn_Program_stack_trace_from_pcs_DOC},
	{"symbols", (PyCFunction)Program_symbols, METH_VARARGS,
	 drgn_Program_symbols_DOC},
	{"symbol", (PyCFunction)Program_symbol, METH_O,
	 drgn_Program_symbol_DOC},
	{"threads", (PyCFunction)Program_threads, METH_NOARGS,
	 drgn_Program_threads_DOC},
	{"thread", (PyCFunction)Program_thread,
	 METH_VARARGS | METH_KEYWORDS, drgn_Program_thread_DOC},
	{"main_thread", (PyCFunction)Program_main_thread, METH_NOARGS,
	 drgn_Program_main_thread_DOC},
	{"crashed_thread", (PyCFunction)Program_crashed_thread, METH_NOARGS,
	 drgn_Program_crashed_thread_DOC},
	{"void_type", (PyCFunction)Program_void_type,
	 METH_VARARGS | METH_KEYWORDS, drgn_Program_void_type_DOC},
	{"int_type", (PyCFunction)Program_int_type,
	 METH_VARARGS | METH_KEYWORDS, drgn_Program_int_type_DOC},
	{"bool_type", (PyCFunction)Program_bool_type,
	 METH_VARARGS | METH_KEYWORDS, drgn_Program_bool_type_DOC},
	{"float_type", (PyCFunction)Program_float_type,
	 METH_VARARGS | METH_KEYWORDS, drgn_Program_float_type_DOC},
	{"struct_type", (PyCFunction)Program_struct_type,
	 METH_VARARGS | METH_KEYWORDS, drgn_Program_struct_type_DOC},
	{"union_type", (PyCFunction)Program_union_type,
	 METH_VARARGS | METH_KEYWORDS, drgn_Program_union_type_DOC},
	{"class_type", (PyCFunction)Program_class_type,
	 METH_VARARGS | METH_KEYWORDS, drgn_Program_class_type_DOC},
	{"enum_type", (PyCFunction)Program_enum_type,
	 METH_VARARGS | METH_KEYWORDS, drgn_Program_enum_type_DOC},
	{"typedef_type", (PyCFunction)Program_typedef_type,
	 METH_VARARGS | METH_KEYWORDS, drgn_Program_typedef_type_DOC},
	{"pointer_type", (PyCFunction)Program_pointer_type,
	 METH_VARARGS | METH_KEYWORDS, drgn_Program_pointer_type_DOC},
	{"array_type", (PyCFunction)Program_array_type,
	 METH_VARARGS | METH_KEYWORDS, drgn_Program_array_type_DOC},
	{"function_type", (PyCFunction)Program_function_type,
	 METH_VARARGS | METH_KEYWORDS, drgn_Program_function_type_DOC},
	{"_log", (PyCFunction)Program__log, METH_VARARGS},
	{},
};

static PyMemberDef Program_members[] = {
	{"cache", T_OBJECT_EX, offsetof(Program, cache), 0,
	 drgn_Program_cache_DOC},
	{"config", T_OBJECT_EX, offsetof(Program, config), 0,
	 drgn_Program_config_DOC},
	{},
};

static PyGetSetDef Program_getset[] = {
	{"flags", (getter)Program_get_flags, NULL, drgn_Program_flags_DOC},
	{"platform", (getter)Program_get_platform, NULL,
	 drgn_Program_platform_DOC},
	{"core_dump_path", (getter)Program_get_core_dump_path, NULL,
	 drgn_Program_core_dump_path_DOC},
	{"language", (getter)Program_get_language, (setter)Program_set_language,
	 drgn_Program_language_DOC},
	{"debug_info_options", (getter)Program_get_debug_info_options,
	 (setter)Program_set_debug_info_options,
	 drgn_Program_debug_info_options_DOC},
	{},
};

static PyMappingMethods Program_as_mapping = {
	.mp_subscript = (binaryfunc)Program_subscript,
};

static PySequenceMethods Program_as_sequence = {
	.sq_contains = (objobjproc)Program_contains,
};

PyTypeObject Program_type = {
	PyVarObject_HEAD_INIT(NULL, 0)
	.tp_name = "_drgn.Program",
	.tp_basicsize = sizeof(Program),
	.tp_dealloc = (destructor)Program_dealloc,
	.tp_as_sequence = &Program_as_sequence,
	.tp_as_mapping = &Program_as_mapping,
	.tp_flags = Py_TPFLAGS_DEFAULT | Py_TPFLAGS_HAVE_GC,
	.tp_doc = drgn_Program_DOC,
	.tp_traverse = (traverseproc)Program_traverse,
	.tp_clear = (inquiry)Program_clear,
	.tp_methods = Program_methods,
	.tp_members = Program_members,
	.tp_getset = Program_getset,
	.tp_new = (newfunc)Program_new,
};

Program *program_from_core_dump(PyObject *self, PyObject *args, PyObject *kwds)
{
	static char *keywords[] = {"path", NULL};
	struct drgn_error *err;
	PATH_ARG(path, .allow_fd = true);
	if (!PyArg_ParseTupleAndKeywords(args, kwds,
					 "O&:program_from_core_dump", keywords,
					 path_converter, &path))
		return NULL;

	_cleanup_pydecref_ Program *prog =
		(Program *)PyObject_CallObject((PyObject *)&Program_type, NULL);
	if (!prog)
		return NULL;

	if (path.fd >= 0)
		err = drgn_program_init_core_dump_fd(&prog->prog, path.fd);
	else
		err = drgn_program_init_core_dump(&prog->prog, path.path);
	if (err)
		return set_drgn_error(err);
	return_ptr(prog);
}

Program *program_from_kernel(PyObject *self)
{
	struct drgn_error *err;
	_cleanup_pydecref_ Program *prog =
		(Program *)PyObject_CallObject((PyObject *)&Program_type, NULL);
	if (!prog)
		return NULL;
	err = drgn_program_init_kernel(&prog->prog);
	if (err)
		return set_drgn_error(err);
	return_ptr(prog);
}

Program *program_from_pid(PyObject *self, PyObject *args, PyObject *kwds)
{
	struct drgn_error *err;
	static char *keywords[] = {"pid", NULL};
	int pid;
	if (!PyArg_ParseTupleAndKeywords(args, kwds, "i:program_from_pid",
					 keywords, &pid))
		return NULL;

	_cleanup_pydecref_ Program *prog =
		(Program *)PyObject_CallObject((PyObject *)&Program_type, NULL);
	if (!prog)
		return NULL;
	err = drgn_program_init_pid(&prog->prog, pid);
	if (err)
		return set_drgn_error(err);
	return_ptr(prog);
}
