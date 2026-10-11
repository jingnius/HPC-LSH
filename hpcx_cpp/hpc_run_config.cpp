#include "hpc_run_config.h"

#include <fstream>
#include <regex>

namespace {

bool read_json_int_field(const std::string& text, const char* field, int* out) {
    const std::regex re(std::string("\"") + field + R"("\s*:\s*(-?\d+))");
    std::smatch m;
    if (std::regex_search(text, m, re) && m.size() >= 2) {
        *out = std::stoi(m[1].str());
        return true;
    }
    return false;
}

bool read_json_bool_field(const std::string& text, const char* field, bool* out) {
    const std::regex re(std::string("\"") + field + R"("\s*:\s*(true|false))", std::regex::icase);
    std::smatch m;
    if (std::regex_search(text, m, re) && m.size() >= 2) {
        const std::string v = m[1].str();
        *out = (v == "true" || v == "True");
        return true;
    }
    return false;
}

bool read_json_double_field(const std::string& text, const char* field, double* out) {
    const std::regex re(std::string("\"") + field + R"("\s*:\s*([-+]?[0-9]*\.?[0-9]+))");
    std::smatch m;
    if (std::regex_search(text, m, re) && m.size() >= 2) {
        *out = std::stod(m[1].str());
        return true;
    }
    return false;
}

}  // namespace

bool hpc_load_partition_config(const std::string& run_dir, HpcPartitionBuildConfig* cfg, std::string* err) {
    if (cfg == nullptr) {
        if (err) {
            *err = "null cfg";
        }
        return false;
    }
    const std::string path = run_dir + "/hpc_params.json";
    std::ifstream in(path);
    if (!in) {
        if (err) {
            *err = "missing hpc_params.json: " + path;
        }
        return false;
    }
    std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    int iv = 0;
    bool bv = false;
    double dv = 0.0;
    if (read_json_int_field(text, "depth", &iv)) {
        cfg->num_hpc_layers = iv;
    }
    if (read_json_int_field(text, "min_cols", &iv)) {
        cfg->min_cols = iv;
    }
    if (read_json_int_field(text, "num_segments", &iv)) {
        cfg->root_num_segments = iv;
    }
    if (read_json_int_field(text, "adaptive_k_min", &iv)) {
        cfg->adaptive_k_min = iv;
    }
    if (read_json_int_field(text, "adaptive_k_max", &iv)) {
        cfg->adaptive_k_max = iv;
    }
    if (read_json_int_field(text, "initial_split_dims", &iv)) {
        cfg->initial_split_dims = iv;
    }
    if (read_json_int_field(text, "max_split_dims", &iv)) {
        cfg->max_split_dims = iv;
    }
    if (read_json_bool_field(text, "adaptive_num_segments", &bv)) {
        cfg->adaptive_num_segments = bv;
    }
    if (read_json_double_field(text, "max_child_ratio", &dv)) {
        cfg->max_child_ratio = dv;
    }
    return true;
}

bool hpc_load_run_build_config(const std::string& run_dir, HpcRunBuildConfig* cfg, std::string* err) {
    if (cfg == nullptr) {
        if (err) {
            *err = "null cfg";
        }
        return false;
    }
    if (!hpc_load_partition_config(run_dir, &cfg->partition, err)) {
        return false;
    }
    const std::string path = run_dir + "/hpc_params.json";
    std::ifstream in(path);
    if (!in) {
        if (err) {
            *err = "missing hpc_params.json: " + path;
        }
        return false;
    }
    std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    int iv = 0;
    if (read_json_int_field(text, "route_r", &iv)) {
        cfg->route_r = iv;
    }
    return true;
}
