#include "Python.h"

#include "wasm3.h"
#include "m3_api_defs.h"

#define MAX_ARGS 32

typedef struct {
    PyObject_HEAD
    IM3Environment e;
} m3_environment;

typedef struct {
    PyObject_HEAD
    m3_environment *env;
    IM3Runtime r;
} m3_runtime;

typedef struct {
    PyObject_HEAD
    m3_environment *env;
    IM3Module m;
    //bool is_gas_metered;
    int64_t total_gas;
    int64_t current_gas;
} m3_module;

typedef struct {
    PyObject_HEAD
    IM3Function f;
    IM3Runtime r;
} m3_function;

static PyObject *M3_Environment_Type;
static PyObject *M3_Runtime_Type;
static PyObject *M3_Module_Type;
static PyObject *M3_Function_Type;


m3ApiRawFunction(metering_usegas)
{
    m3ApiGetArg     (int32_t, gas)

    m3_module *mod = (m3_module *)(_ctx->userdata);

    mod->current_gas -= gas;

    if (UNLIKELY(mod->current_gas < 0)) {
        m3ApiTrap("[trap] Out of gas");
    }
    m3ApiSuccess();
}


static m3_environment*
newEnvironment(PyObject *arg)
{
    m3_environment *self = PyObject_New(m3_environment, (PyTypeObject*)M3_Environment_Type);
    if (!self) return NULL;
    self->e = m3_NewEnvironment();
    return self;    
}

static void
delEnvironment(m3_environment *self)
{
    m3_FreeEnvironment(self->e);
}

static PyObject *
formatError(PyObject *exception, IM3Runtime runtime, M3Result err)
{
    M3ErrorInfo info;
    memset(&info, 0, sizeof(info));
    m3_GetErrorInfo (runtime, &info);
    if (strlen(info.message)) {
        PyErr_Format(exception, "%s (%s)", err, info.message);
    } else {
        PyErr_SetString(exception, err);
    }
    return NULL;
}

static void
put_arg_on_stack(u64 *s, M3ValueType type, PyObject *arg)
{
    switch (type) {
        case c_m3Type_i32:  *(i32*)(s) = PyLong_AsLong(arg);     break;
        case c_m3Type_i64:  *(i64*)(s) = PyLong_AsLongLong(arg); break;
        case c_m3Type_f32:  *(f32*)(s) = PyFloat_AsDouble(arg);  break;
        case c_m3Type_f64:  *(f64*)(s) = PyFloat_AsDouble(arg);  break;
    }
}

static PyObject *
get_arg_from_stack(u64 *s, M3ValueType type)
{
    switch (type) {
        case c_m3Type_i32:  return PyLong_FromLong(     *(i32*)s);   break;
        case c_m3Type_i64:  return PyLong_FromLongLong( *(i64*)s);   break;
        case c_m3Type_f32:  return PyFloat_FromDouble(  *(f32*)s);   break;
        case c_m3Type_f64:  return PyFloat_FromDouble(  *(f64*)s);   break;
        default:
            return PyErr_Format(PyExc_TypeError, "unknown type %d", (int)type);
    }
}

static PyObject *
M3_Environment_new_runtime(m3_environment *env, PyObject *stack_size_bytes)
{
    size_t n = PyLong_AsSize_t(stack_size_bytes);
    m3_runtime *self = PyObject_New(m3_runtime, (PyTypeObject*)M3_Runtime_Type);
    if (!self) return NULL;
    Py_INCREF(env);
    self->env = env;
    self->r = m3_NewRuntime(env->e, n, NULL);
    return self;
}

static PyObject *
M3_Environment_parse_module(m3_environment *env, PyObject *bytes)
{
    Py_ssize_t size;
    char *data;
    PyBytes_AsStringAndSize(bytes, &data, &size);
    IM3Module m;
    M3Result err = m3_ParseModule(env->e, &m, data, size);
    if (err) {
        PyErr_SetString(PyExc_RuntimeError, err);
        return NULL;
    }
    Py_INCREF(bytes);

    m3_module *self = PyObject_New(m3_module, (PyTypeObject*)M3_Module_Type);
    if (!self) return NULL;
    Py_INCREF(env);
    self->env = env;
    self->m = m;
    self->total_gas = self->current_gas = 0;
    return self;
}

static PyMethodDef M3_Environment_methods[] = {
    {"new_runtime",            (PyCFunction)M3_Environment_new_runtime,  METH_O,
        PyDoc_STR("new_runtime(stack_size_bytes) -> Runtime")},
    {"parse_module",            (PyCFunction)M3_Environment_parse_module,  METH_O,
        PyDoc_STR("new_runtime(bytes) -> Module")},
    {NULL,              NULL}           /* sentinel */
};

static PyType_Slot M3_Environment_Type_slots[] = {
    {Py_tp_doc, "The wasm3.Environment type"},
    {Py_tp_finalize, delEnvironment},
    {Py_tp_new, newEnvironment},
    {Py_tp_methods, M3_Environment_methods},
    {0, 0}
};

static PyObject *
M3_Runtime_load(m3_runtime *runtime, PyObject *arg)
{
    m3_module *module = (m3_module *)arg;
    M3Result err = m3_LoadModule(runtime->r, module->m);
    if (err) {
        return formatError(PyExc_RuntimeError, runtime->r, err);
    }

    err = m3_LinkRawFunctionEx (module->m, "metering", "usegas", "v(i)", &metering_usegas, module);
    /*if (!err) {
        self->is_gas_metered = true;
    }*/
    if (err && err != m3Err_functionLookupFailed) {
        return formatError(PyExc_RuntimeError, m3_GetModuleRuntime(module->m), err);
    }

    Py_RETURN_NONE;
}

static PyObject *
M3_Runtime_find_function(m3_runtime *runtime, PyObject *name)
{
    IM3Function func = NULL;
    M3Result err = m3_FindFunction(&func, runtime->r, PyUnicode_AsUTF8(name));
    if (err) {
        return formatError(PyExc_RuntimeError, runtime->r, err);
    }
    m3_function *self = PyObject_New(m3_function, (PyTypeObject*)M3_Function_Type);
    if (!self) return NULL;
	Py_INCREF(runtime);
    self->f = func;
    self->r = runtime->r;
    return self;
}

static PyObject *
M3_Runtime_get_memory(m3_runtime *runtime, PyObject *index)
{
    Py_buffer* pybuff;
    uint32_t size = 0;
    uint8_t *mem = m3_GetMemory(runtime->r, &size, PyLong_AsLong(index));
    if (!mem)
        Py_RETURN_NONE;

    pybuff = (Py_buffer*) PyMem_Malloc(sizeof(Py_buffer));
    PyBuffer_FillInfo(pybuff, (PyObject *)runtime, mem, size, 0, PyBUF_WRITABLE);
    return PyMemoryView_FromBuffer(pybuff);
}

static PyMethodDef M3_Runtime_methods[] = {
    {"load",            (PyCFunction)M3_Runtime_load,  METH_O,
        PyDoc_STR("load(module) -> None")},
    {"find_function", (PyCFunction)M3_Runtime_find_function,  METH_O,
        PyDoc_STR("find_function(name) -> Function")},
    {"get_memory",     (PyCFunction)M3_Runtime_get_memory,  METH_O,
        PyDoc_STR("get_memory(index) -> memoryview")},
    {NULL,              NULL}           /* sentinel */
};

static PyType_Slot M3_Runtime_Type_slots[] = {
    {Py_tp_doc, "The wasm3.Runtime type"},
    // {Py_tp_finalize, delRuntime},
    // {Py_tp_new, newRuntime},
    {Py_tp_methods, M3_Runtime_methods},
    {0, 0}
};

static PyObject *
Module_name(m3_module *self, void * closure)
{
    return PyUnicode_FromString(m3_GetModuleName(self->m));
}

static int
Module_setGasLimit(m3_module *self, PyObject *value, void * closure)
{
    self->total_gas = PyFloat_AsDouble(value)*10000.0;
    self->current_gas = self->total_gas;
    return 0;
}

static PyObject *
Module_getGasLimit(m3_module *self, void * closure)
{
    return PyFloat_FromDouble((double)(self->total_gas)/10000.0);
}

static PyObject *
Module_getGasUsed(m3_module *self, void * closure)
{
    return PyFloat_FromDouble((double)(self->total_gas - self->current_gas)/10000.0);
}

static const char* trapException = "function raised exception";

m3ApiRawFunction(CallImport)
{
    PyObject *pFunc = (PyObject *)(_ctx->userdata);
    IM3Function f = _ctx->function;
    int nArgs = m3_GetArgCount(f);
    int nRets = m3_GetRetCount(f);
    PyObject *pArgs = PyTuple_New(nArgs);
    if (!pArgs) {
        m3ApiTrap("python call: args not allocated");
    }

    for (Py_ssize_t i = 0; i < nArgs; ++i) {
        PyObject *arg = get_arg_from_stack(&_sp[i+nRets], m3_GetArgType(f, i));
        PyTuple_SET_ITEM(pArgs, i, arg);
    }

    PyObject * pRets = PyObject_CallObject(pFunc, pArgs);
    if (!pRets) m3ApiTrap(trapException);

    if (PyTuple_Check(pRets)) {
        if (PyTuple_GET_SIZE(pRets) != nRets) {
            m3ApiTrap("python call: return tuple length mismatch");
        }
        for (Py_ssize_t i = 0; i < nRets; ++i) {
            PyObject *ret = PyTuple_GET_ITEM(pRets, i);
            if (!ret) m3ApiTrap("python call: return type invalid");
            put_arg_on_stack(&_sp[i], m3_GetRetType(f, i), ret);
        }
    } else {
        if (nRets == 0) {
            if (pRets != Py_None) {
                //m3ApiTrap("python call: return value ignored");
            }
        } else if (nRets == 1) {
            if (pRets == Py_None) {
                m3ApiTrap("python call: should return a value");
            }
            put_arg_on_stack(&_sp[0], m3_GetRetType(f, 0), pRets);
        } else {
            m3ApiTrap("python call: should return a tuple");
        }
    }
    m3ApiSuccess();
}

static PyObject *
M3_Module_link_function(m3_module *self, PyObject *args)
{
    PyObject *mod_name, *func_name, *func_sig, *pFunc;
    if (PyTuple_Size(args) == 4) {
        mod_name  = PyTuple_GET_ITEM(args, 0);
        func_name = PyTuple_GET_ITEM(args, 1);
        func_sig  = PyTuple_GET_ITEM(args, 2);
        pFunc     = PyTuple_GET_ITEM(args, 3);
    } else if (PyTuple_Size(args) == 3) {
        mod_name  = PyTuple_GET_ITEM(args, 0);
        func_name = PyTuple_GET_ITEM(args, 1);
        func_sig  = NULL;
        pFunc     = PyTuple_GET_ITEM(args, 2);
    } else {
        PyErr_SetString(PyExc_TypeError, "link_function takes 3 or 4 arguments");
        return NULL;
    }

    if (!PyCallable_Check(pFunc)) {
        PyErr_SetString(PyExc_TypeError, "function should be a callable object");
        return NULL;
    }
    M3Result err = m3_LinkRawFunctionEx (self->m, PyUnicode_AsUTF8(mod_name), PyUnicode_AsUTF8(func_name),
                                         (func_sig?PyUnicode_AsUTF8(func_sig):NULL), CallImport, pFunc);
    if (err && err != m3Err_functionLookupFailed) {
        return formatError(PyExc_RuntimeError, m3_GetModuleRuntime(self->m), err);
    }

    Py_INCREF(pFunc);
    Py_RETURN_NONE;
}

static PyObject *
M3_Module_get_global(m3_module *self, PyObject *name)
{
    M3TaggedValue tagged;
    IM3Global g = m3_FindGlobal(self->m, PyUnicode_AsUTF8(name));
    M3Result err = m3_GetGlobal (g, &tagged);
    if (err) {
        return formatError(PyExc_RuntimeError, m3_GetModuleRuntime(self->m), err);
    }
    switch (tagged.type) {
        case c_m3Type_i32:  return PyLong_FromLong(     tagged.value.i32);   break;
        case c_m3Type_i64:  return PyLong_FromLongLong( tagged.value.i64);   break;
        case c_m3Type_f32:  return PyFloat_FromDouble(  tagged.value.f32);   break;
        case c_m3Type_f64:  return PyFloat_FromDouble(  tagged.value.f64);   break;
        default:            return PyErr_Format(PyExc_TypeError, "unknown type %d", (int)tagged.type);
    }
}

static PyObject *
M3_Module_set_global(m3_module *self, PyObject *args)
{
    if (PyTuple_Size(args) != 2) {
        PyErr_SetString(PyExc_TypeError, "set_global takes 2 arguments");
        return NULL;
    }

    PyObject *name  = PyTuple_GET_ITEM(args, 0);
    PyObject *value = PyTuple_GET_ITEM(args, 1);

    IM3Global g = m3_FindGlobal(self->m, PyUnicode_AsUTF8(name));

    M3TaggedValue tagged = {
        .type      = m3_GetGlobalType(g)
    };

    switch (tagged.type) {
        case c_m3Type_i32:  tagged.value.i32 = PyLong_AsLong(value);        break;
        case c_m3Type_i64:  tagged.value.i64 = PyLong_AsLongLong(value);    break;
        case c_m3Type_f32:  tagged.value.f32 = PyFloat_AsDouble(value);     break;
        case c_m3Type_f64:  tagged.value.f64 = PyFloat_AsDouble(value);     break;
        default:            return PyErr_Format(PyExc_TypeError, "unknown type %d", (int)tagged.type);
    }

    M3Result err = m3_SetGlobal (g, &tagged);

    if (err) {
        return formatError(PyExc_RuntimeError, m3_GetModuleRuntime(self->m), err);
    }

    Py_RETURN_NONE;
}

static PyGetSetDef M3_Module_properties[] = {
    {"name",        (getter) Module_name, NULL, "module name", NULL},
    {"gasLimit",    (getter) Module_getGasLimit, (setter) Module_setGasLimit, "gas limit for metered modules", NULL},
    {"gasUsed",     (getter) Module_getGasUsed, NULL, "gas used", NULL},
    {0},
};

static PyMethodDef M3_Module_methods[] = {
    {"link_function", (PyCFunction)M3_Module_link_function,  METH_VARARGS,
        PyDoc_STR("link_function(module, name, signature, function)")},

    {"get_global", (PyCFunction)M3_Module_get_global,  METH_O,
        PyDoc_STR("get_global(name) -> value")},

    {"set_global", (PyCFunction)M3_Module_set_global,  METH_VARARGS,
        PyDoc_STR("set_global(name, value)")},

    {NULL,              NULL}           /* sentinel */
};

static PyType_Slot M3_Module_Type_slots[] = {
    {Py_tp_doc, "The wasm3.Module type"},
    // {Py_tp_finalize, delModule},
    // {Py_tp_new, newModule},
    {Py_tp_methods, M3_Module_methods},
    {Py_tp_getset, M3_Module_properties},
    {0, 0}
};

static PyObject *
get_result_from_stack(m3_function *func)
{
    int nRets = m3_GetRetCount(func->f);
    if (nRets <= 0) {
        Py_RETURN_NONE;
    }
    
    if (nRets > MAX_ARGS) {
        PyErr_SetString(PyExc_RuntimeError, "too many rets");
        return NULL;
    }

    static uint64_t    valbuff[MAX_ARGS];
    static const void* valptrs[MAX_ARGS];
    memset(valbuff, 0, sizeof(valbuff));
    memset(valptrs, 0, sizeof(valptrs));

    for (int i = 0; i < nRets; i++) {
        valptrs[i] = &valbuff[i];
    }
    M3Result err = m3_GetResults (func->f, nRets, valptrs);
    if (err) {
        return formatError(PyExc_RuntimeError, func->r, err);
    }

    if (nRets == 1) {
        return get_arg_from_stack(valptrs[0], m3_GetRetType(func->f, 0));
    } else {
        PyObject *ret = PyTuple_New(nRets);
        if (ret) {
            Py_ssize_t i;
            for (i = 0; i < nRets; ++i) {
                PyObject *val = get_arg_from_stack(valptrs[i], m3_GetRetType(func->f, i));
                PyTuple_SET_ITEM(ret, i, val);
            }
        }
        return ret;
    }
}

static
void print_backtrace(IM3Runtime runtime)
{
    IM3BacktraceInfo info = m3_GetBacktrace(runtime);
    if (!info) {
        return;
    }

    fprintf(stderr, "==== wasm backtrace:");

    int frameCount = 0;
    IM3BacktraceFrame curr = info->frames;
    while (curr)
    {
        fprintf(stderr, "\n  %d: 0x%06x - %s!%s",
                           frameCount, curr->moduleOffset,
                           m3_GetModuleName (m3_GetFunctionModule(curr->function)),
                           m3_GetFunctionName (curr->function)
               );
        curr = curr->next;
        frameCount++;
    }
    if (info->lastFrame == M3_BACKTRACE_TRUNCATED) {
        fprintf(stderr, "\n  (truncated)");
    }
    fprintf(stderr, "\n");
}

static PyObject *
M3_Function_call_argv(m3_function *func, PyObject *args)
{
    Py_ssize_t size = PyTuple_GET_SIZE(args);
    const char* argv[MAX_ARGS];
    for(Py_ssize_t i = 0; i< size;++i) {
        PyObject *arg = PyTuple_GET_ITEM(args, i);
        if (!PyUnicode_Check(arg)) {
            PyErr_SetString(PyExc_RuntimeError, "all arguments should be strings");
            return NULL;
        }
        argv[i] = PyUnicode_AsUTF8(arg);
    }
    M3Result err = m3_CallArgv(func->f, size, argv);
    if (err == trapException) {
        return NULL;
    } else if (err) {
        print_backtrace(func->r);
        return formatError(PyExc_RuntimeError, func->r, err);
    }

    return get_result_from_stack(func);
}

static PyObject*
M3_Function_call(m3_function *self, PyObject *args, PyObject *kwargs)
{
    IM3Function f = self->f;

    int nArgs = m3_GetArgCount(f);

    if (nArgs > MAX_ARGS) {
        PyErr_SetString(PyExc_RuntimeError, "too many args");
        return NULL;
    }

    static uint64_t    valbuff[MAX_ARGS];
    static const void* valptrs[MAX_ARGS];
    memset(valbuff, 0, sizeof(args));
    memset(valptrs, 0, sizeof(valptrs));

    for (int i = 0; i < nArgs; i++) {
        u64* s = &valbuff[i];
        valptrs[i] = s;
        put_arg_on_stack(s, m3_GetArgType(f, i), PyTuple_GET_ITEM(args, i));
    }

    M3Result err = m3_Call (f, nArgs, valptrs);
    if (err == trapException) {
        return NULL;
    } else if (err) {
        print_backtrace(self->r);
        return formatError(PyExc_RuntimeError, self->r, err);
    }

    return get_result_from_stack(self);
}

static PyObject*
Function_name(m3_function *self, void * closure)
{
    return PyUnicode_FromString(m3_GetFunctionName(self->f));
}

static PyObject*
Function_num_args(m3_function *self, void * closure)
{
    return PyLong_FromLong(m3_GetArgCount(self->f));
}

static PyObject*
Function_num_rets(m3_function *self, void * closure)
{
    return PyLong_FromLong(m3_GetRetCount(self->f));
}

static PyObject*
Function_arg_types(m3_function *self, void * closure)
{
    Py_ssize_t nArgs = m3_GetArgCount(self->f);
    PyObject *ret = PyTuple_New(nArgs);
    if (ret) {
        Py_ssize_t i;
        for (i = 0; i < nArgs; ++i) {
            PyTuple_SET_ITEM(ret, i, PyLong_FromLong(m3_GetArgType(self->f, i)));
        }
    }
    return ret;
}

static PyObject*
Function_ret_types(m3_function *self, void * closure)
{
    Py_ssize_t nRets = m3_GetRetCount(self->f);
    PyObject *ret = PyTuple_New(nRets);
    if (ret) {
        Py_ssize_t i;
        for (i = 0; i < nRets; ++i) {
            PyTuple_SET_ITEM(ret, i, PyLong_FromLong(m3_GetRetType(self->f, i)));
        }
    }
    return ret;
}

static PyGetSetDef M3_Function_properties[] = {
    {"name", (getter) Function_name, NULL, "function name", NULL },
    {"num_args", (getter) Function_num_args, NULL, "number of args", NULL },
    {"num_rets", (getter) Function_num_rets, NULL, "number of rets", NULL },
    {"arg_types", (getter) Function_arg_types, NULL, "types of args", NULL },
    {"ret_types", (getter) Function_ret_types, NULL, "types of rets", NULL },
    {NULL}  /* Sentinel */
};

static PyMethodDef M3_Function_methods[] = {
    {"call_argv", (PyCFunction)M3_Function_call_argv,  METH_VARARGS,
        PyDoc_STR("call_argv(args...) -> result")},
    {NULL, NULL}           /* sentinel */
};

static PyType_Slot M3_Function_Type_slots[] = {
    {Py_tp_doc, "The wasm3.Function type"},
    // {Py_tp_finalize, delFunction},
    // {Py_tp_new, newFunction},
    {Py_tp_call, M3_Function_call},
    {Py_tp_methods, M3_Function_methods},
    {Py_tp_getset, M3_Function_properties},
    {0, 0}
};

static PyType_Spec M3_Environment_Type_spec = {
    "wasm3.Environment",
    sizeof(m3_environment),
    0,
    Py_TPFLAGS_DEFAULT | Py_TPFLAGS_BASETYPE,
    M3_Environment_Type_slots
};

static PyType_Spec M3_Runtime_Type_spec = {
    "wasm3.Runtime",
    sizeof(m3_runtime),
    0,
    Py_TPFLAGS_DEFAULT | Py_TPFLAGS_BASETYPE,
    M3_Runtime_Type_slots
};

static PyType_Spec M3_Module_Type_spec = {
    "wasm3.Module",
    sizeof(m3_module),
    0,
    Py_TPFLAGS_DEFAULT | Py_TPFLAGS_BASETYPE,
    M3_Module_Type_slots
};

static PyType_Spec M3_Function_Type_spec = {
    "wasm3.Function",
    sizeof(m3_function),
    0,
    Py_TPFLAGS_DEFAULT | Py_TPFLAGS_BASETYPE,
    M3_Function_Type_slots
};

static int
m3_modexec(PyObject *m)
{
    M3_Environment_Type = PyType_FromSpec(&M3_Environment_Type_spec);
    if (M3_Environment_Type == NULL)
        goto fail;
    M3_Runtime_Type = PyType_FromSpec(&M3_Runtime_Type_spec);
    if (M3_Runtime_Type == NULL)
        goto fail;
    M3_Module_Type = PyType_FromSpec(&M3_Module_Type_spec);
    if (M3_Module_Type == NULL)
        goto fail;
    M3_Function_Type = PyType_FromSpec(&M3_Function_Type_spec);
    if (M3_Function_Type == NULL)
        goto fail;
    PyModule_AddStringMacro(m, M3_VERSION);
    PyModule_AddObject(m, "Environment", M3_Environment_Type);
    PyModule_AddObject(m, "Runtime", M3_Runtime_Type);
    PyModule_AddObject(m, "Module", M3_Module_Type);
    PyModule_AddObject(m, "Function", M3_Function_Type);
    return 0;
 fail:
    Py_XDECREF(m);
    return -1;
}

static PyModuleDef_Slot m3_slots[] = {
    {Py_mod_exec, m3_modexec},
    {0, NULL}
};

PyDoc_STRVAR(m3_doc,
"wasm3 python bindings");

static struct PyModuleDef m3module = {
    PyModuleDef_HEAD_INIT,
    "wasm3",
    m3_doc,
    0,
    0, // methods
    m3_slots,
    NULL,
    NULL,
    NULL
};

PyMODINIT_FUNC
PyInit_wasm3(void)
{
    return PyModuleDef_Init(&m3module);
}

