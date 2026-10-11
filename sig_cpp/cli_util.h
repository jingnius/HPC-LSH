#pragma once
#include <string>
#include <vector>

struct ArgMap {
  std::vector<std::string> positional;
  bool help = false;
  bool compare = false;
  std::string cybuf;
  std::string inver;
  std::string dict;
  std::string target;
  std::string out;
  std::string out_dir;
  std::string sample_order_out;
  std::string sample_order;
  std::string compare_a;
  std::string compare_b;
  int band = 0;
  uint32_t seed = 0;
  bool skip_inver = false;
  int num_perm = 5;
};

bool parse_args(int argc, char** argv, ArgMap* m, std::string* err);
void print_json_line(const std::string& json);
