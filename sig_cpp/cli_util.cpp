#include "cli_util.h"

#include <iostream>
#include <sstream>
#include <unordered_map>

bool parse_args(int argc, char** argv, ArgMap* m, std::string* err) {
  if (!m) return false;
  for (int i = 1; i < argc; ++i) {
    std::string a = argv[i];
    if (a == "--help" || a == "-h") {
      m->help = true;
      continue;
    }
    if (a == "--compare") {
      m->compare = true;
      if (i + 2 >= argc) {
        if (err) *err = "compare requires two paths";
        return false;
      }
      m->compare_a = argv[++i];
      m->compare_b = argv[++i];
      continue;
    }
    auto need = [&](const char* flag, std::string* dst) -> bool {
      if (i + 1 >= argc) {
        if (err) *err = std::string("missing value for ") + flag;
        return false;
      }
      *dst = argv[++i];
      return true;
    };
    if (a == "--cybuf") {
      if (!need("--cybuf", &m->cybuf)) return false;
    } else if (a == "--inver") {
      if (!need("--inver", &m->inver)) return false;
    } else if (a == "--dict") {
      if (!need("--dict", &m->dict)) return false;
    } else if (a == "--target") {
      if (!need("--target", &m->target)) return false;
    } else if (a == "--out") {
      if (!need("--out", &m->out)) return false;
    } else if (a == "--out-dir") {
      if (!need("--out-dir", &m->out_dir)) return false;
    } else if (a == "--sample-order-out") {
      if (!need("--sample-order-out", &m->sample_order_out)) return false;
    } else if (a == "--sample-order") {
      if (!need("--sample-order", &m->sample_order)) return false;
    } else if (a == "--band") {
      if (i + 1 >= argc) {
        if (err) *err = "missing band";
        return false;
      }
      m->band = std::stoi(argv[++i]);
    } else if (a == "--seed") {
      if (i + 1 >= argc) {
        if (err) *err = "missing seed";
        return false;
      }
      m->seed = static_cast<uint32_t>(std::stoul(argv[++i]));
    } else if (a == "--skip-inver") {
      m->skip_inver = true;
    } else if (a == "--num-perm") {
      if (!need("--num-perm", &m->target)) return false;  // unused placeholder
    } else {
      m->positional.push_back(a);
    }
  }
  return true;
}

void print_json_line(const std::string& json) { std::cout << json << '\n'; }
