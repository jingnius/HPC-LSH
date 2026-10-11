#include "cli_util.h"
#include "cybuf_core.h"

#include <chrono>
#include <cstdlib>
#include <iostream>
#include <sstream>

static void usage() {
  std::cerr
      << "Usage:\n"
      << "  sig_build_cli --cybuf CYBF.bin [--inver INVR.bin] --seed N [--band B] --out SIGB.bin "
         "[--sample-order-out SORD.bin]\n"
      << "  sig_build_cli --compare REF.bin CAND.bin\n"
      << "Env: SIG_CYBUF_MMAP, SIG_LAZY_INVER, SIG_INVER_MMAP, SIG_STREAM_COLUMNS, SIG_STREAM_BATCH_SIZE\n";
}

static std::string json_escape(const std::string& s) {
  std::string o;
  for (char c : s) {
    if (c == '\\' || c == '"') o.push_back('\\');
    o.push_back(c);
  }
  return o;
}

static std::string sibling_inver(const std::string& cybuf_path) {
  const std::size_t slash = cybuf_path.find_last_of('/');
  const std::string dir = (slash == std::string::npos) ? std::string() : cybuf_path.substr(0, slash + 1);
  return dir + "inver.native.bin";
}

int main(int argc, char** argv) {
  ArgMap args;
  std::string err;
  if (!parse_args(argc, argv, &args, &err)) {
    std::cerr << err << '\n';
    return 2;
  }
  if (args.help) {
    usage();
    return 0;
  }

  if (args.compare) {
    SigBand a;
    SigBand b;
    if (!sig_read_native(args.compare_a, &a, &err)) {
      std::cerr << "compare failed: " << err << '\n';
      return 1;
    }
    if (!sig_read_native(args.compare_b, &b, &err)) {
      std::cerr << "compare failed: " << err << '\n';
      return 1;
    }
    const bool ok = sig_compare(a, b, &err);
    std::ostringstream js;
    js << "{\"match\":" << (ok ? "true" : "false") << ",\"n_cols\":" << a.n_cols;
    if (!ok) js << ",\"error\":\"" << json_escape(err) << "\"";
    js << "}";
    print_json_line(js.str());
    return ok ? 0 : 1;
  }

  if (args.cybuf.empty() || args.out.empty()) {
    usage();
    return 2;
  }

  CybufAux cy;
  const auto t0 = std::chrono::steady_clock::now();
  if (!cybuf_read_native(args.cybuf, &cy, &err)) {
    std::cerr << "cybuf read failed: " << err << '\n';
    return 1;
  }

  SigBuildOptions opts{};
  std::string inver_path = args.inver.empty() ? sibling_inver(args.cybuf) : args.inver;
  opts.inver_path = inver_path.c_str();
  opts.stream_out_path = args.out.c_str();

  SigBand band;
  std::vector<int32_t> order;
  if (!sig_build(cy, args.band, args.seed, &band, &order, &err, &opts)) {
    std::cerr << "sig build failed: " << err << '\n';
    return 1;
  }
  const auto t1 = std::chrono::steady_clock::now();

  const char* stream_env = std::getenv("SIG_STREAM_COLUMNS");
  const bool stream_on = stream_env == nullptr || stream_env[0] == '\0' || stream_env[0] != '0';
  if (!stream_on) {
    if (!sig_write_native(args.out, band, &err)) {
      std::cerr << "sig write failed: " << err << '\n';
      return 1;
    }
  }
  const auto t2 = std::chrono::steady_clock::now();

  if (!args.sample_order_out.empty()) {
    if (!sample_order_write_native(args.sample_order_out, order, &err)) {
      std::cerr << "sample-order write failed: " << err << '\n';
      return 1;
    }
  }
  const double build_s = std::chrono::duration<double>(t1 - t0).count();
  const double write_s = std::chrono::duration<double>(t2 - t1).count();
  std::ostringstream js;
  js << "{\"path\":\"" << json_escape(args.out) << "\",\"n_cols\":" << band.n_cols
     << ",\"n_samples\":" << band.n_samples << ",\"seed\":" << args.seed << ",\"band\":" << args.band
     << ",\"build_s\":" << build_s << ",\"write_s\":" << write_s << ",\"stream_columns\":" << (stream_on ? "true" : "false");
  if (!args.sample_order_out.empty()) js << ",\"sample_order\":\"" << json_escape(args.sample_order_out) << "\"";
  js << "}";
  print_json_line(js.str());
  return 0;
}
