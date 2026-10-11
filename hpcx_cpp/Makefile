CXX ?= g++
PYTHON_CONFIG ?= python3-config
PYTHON_CFLAGS := $(shell $(PYTHON_CONFIG) --includes 2>/dev/null)
PYTHON_PREFIX := $(shell $(PYTHON_CONFIG) --prefix 2>/dev/null)
PYTHON_LDFLAGS := $(shell $(PYTHON_CONFIG) --ldflags --embed 2>/dev/null) -Wl,-rpath,$(PYTHON_PREFIX)/lib
CXXFLAGS := -O3 -march=native -std=c++17 -Wall -Wextra -fopenmp -I. $(PYTHON_CFLAGS)
LDFLAGS := -lz -fopenmp $(PYTHON_LDFLAGS)

COMMON_SRCS := hpc_npz.cpp hpc_rss.cpp hpc_flat_search.cpp hpc_partition_view.cpp hpc_hpcx.cpp \
	hpc_mgn_compact.cpp hpc_partition_flat.cpp hpc_mgn_flat_local.cpp hpcx_write.cpp

V8_SRCS := hpc_col_build_cache.cpp hpc_partition_bfs_v8.cpp hpc_mgn_build_v8.cpp \
	hpc_v8_compress_flags.cpp hpc_v8_cskey_flags.cpp hpc_v8_stream_checkpoint.cpp hpc_sig_hits_mmap.cpp hpc_one_index_intern.cpp

BUILD_SRCS := hpc_pickle.cpp hpc_zip.cpp hpc_sig_io.cpp hpc_sig_index.cpp hpc_index_one_io.cpp \
	hpc_index_one_store.cpp hpc_ct_ops.cpp hpc_partition_build.cpp hpc_routes_build.cpp hpc_mgn_build.cpp \
	hpc_native_write.cpp hpc_run_config.cpp hpc_py_embed.cpp hpc_native_build.cpp hpc_build.cpp $(V8_SRCS)

.PHONY: all clean
all: hpcx_build_cli hpcx_multiband_pool_cli hpcx_bench_cli

hpcx_build_cli: $(COMMON_SRCS) $(BUILD_SRCS) hpcx_build_cli.cpp
	$(CXX) $(CXXFLAGS) -o $@ $^ $(LDFLAGS)

SIG_QUERY_SRCS := query_encode.cpp ../sig_cpp/dict_io.cpp
hpcx_multiband_pool_cli: $(COMMON_SRCS) hpcx_multiband_pool_cli.cpp $(SIG_QUERY_SRCS)
	$(CXX) $(CXXFLAGS) -I. -I../sig_cpp -o $@ $^ $(LDFLAGS)

clean:
	rm -f hpcx_build_cli hpcx_multiband_pool_cli hpcx_bench_cli


hpcx_bench_cli: $(COMMON_SRCS) hpcx_bench_cli.cpp
	$(CXX) $(CXXFLAGS) -o $@ $^ $(LDFLAGS)
