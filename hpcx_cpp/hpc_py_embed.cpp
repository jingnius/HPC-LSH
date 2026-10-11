#include "hpc_py_embed.h"

#include <Python.h>

#include <mutex>
#include <string>
#include <unistd.h>

namespace {

std::mutex g_py_mutex;
bool g_py_ready = false;

std::string dirname_of(const std::string& path) {
    const std::size_t pos = path.find_last_of('/');
    if (pos == std::string::npos) {
        return ".";
    }
    return path.substr(0, pos);
}

bool setup_python_paths(std::string* err) {
    char buf[4096];
    const ssize_t n = readlink("/proc/self/exe", buf, sizeof(buf) - 1);
    if (n <= 0) {
        if (err) {
            *err = "readlink /proc/self/exe failed";
        }
        return false;
    }
    buf[n] = '\0';
    const std::string exe(buf);
    const std::string hpcx_cpp = dirname_of(exe);
    const std::string scal = dirname_of(hpcx_cpp);
    const std::string proj = dirname_of(scal);
    const std::string hpcx_tools = hpcx_cpp + "/tools";
    const std::string scal_tools = scal + "/tools";
    const std::string minhash0 = proj + "/vendor/minhash0";

    const std::string script =
        std::string("import sys\nfor p in (") +
        "'" + hpcx_tools + "', '" + scal_tools + "', '" + minhash0 + "', proj + '/vendor/sig_build'" +
        "):\n    if p not in sys.path:\n        sys.path.insert(0, p)\n";
    if (PyRun_SimpleString(script.c_str()) != 0) {
        if (err) {
            *err = "failed to configure embedded Python sys.path";
        }
        PyErr_Print();
        return false;
    }
    return true;
}

bool ensure_python(std::string* err) {
    std::lock_guard<std::mutex> lock(g_py_mutex);
    if (!g_py_ready) {
        Py_InitializeEx(0);
        if (!setup_python_paths(err)) {
            return false;
        }
        g_py_ready = true;
    }
    return true;
}

}  // namespace

bool hpc_py_export_hpc_zip_streaming(
    const std::string& sig_native_path,
    const std::string& run_dir,
    int band,
    const std::string& out_zip_path,
    const std::string& index_one_bin,
    const std::string& partition_json,
    std::string* err) {
    (void)sig_native_path;
    if (!ensure_python(err)) {
        return false;
    }

    PyGILState_STATE gstate = PyGILState_Ensure();
    bool ok = false;

    PyObject* main_mod = PyImport_ImportModule("export_hpc_zip_from_sig");
    if (main_mod == nullptr) {
        if (err) {
            *err = "import export_hpc_zip_from_sig failed";
        }
        PyErr_Print();
        PyGILState_Release(gstate);
        return false;
    }

    PyObject* fn = PyObject_GetAttrString(main_mod, "export_hpc_zip");
    if (fn == nullptr || !PyCallable_Check(fn)) {
        if (err) {
            *err = "export_hpc_zip not callable";
        }
        Py_XDECREF(fn);
        Py_DECREF(main_mod);
        PyGILState_Release(gstate);
        return false;
    }

    PyObject* kwargs = PyDict_New();
    PyDict_SetItemString(kwargs, "run_dir", PyUnicode_FromString(run_dir.c_str()));
    PyDict_SetItemString(kwargs, "band", PyLong_FromLong(band));
    PyDict_SetItemString(kwargs, "out_zip", PyUnicode_FromString(out_zip_path.c_str()));
    PyDict_SetItemString(kwargs, "sig_native", PyUnicode_FromString(sig_native_path.c_str()));
    PyDict_SetItemString(kwargs, "stream", Py_True);
    if (!index_one_bin.empty()) {
        PyDict_SetItemString(kwargs, "index_one_bin", PyUnicode_FromString(index_one_bin.c_str()));
    }
    if (!partition_json.empty()) {
        PyDict_SetItemString(kwargs, "partition_json", PyUnicode_FromString(partition_json.c_str()));
    }

    PyObject* empty_args = PyTuple_New(0);
    PyObject* result = PyObject_Call(fn, empty_args, kwargs);
    Py_DECREF(empty_args);
    Py_DECREF(kwargs);
    Py_DECREF(fn);
    Py_DECREF(main_mod);

    if (result == nullptr) {
        if (err) {
            *err = "export_hpc_zip call failed";
        }
        PyErr_Print();
    } else {
        ok = true;
        Py_DECREF(result);
    }

    PyGILState_Release(gstate);
    return ok;
}
