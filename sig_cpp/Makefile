CXX ?= g++
CXXFLAGS ?= -O3 -march=native -std=c++17 -pthread -Wall -Wextra
CLIS = prep_cli dict_build_cli inver_build_cli cybuf_pack_cli sig_build_cli
LIB_OBJS = cli_util.o dict_io.o inver_io.o target_json.o cybuf_core.o cybuf_pack.o dict_build.o inver_build.o simdjson.o
.PHONY: all clean
all: libs $(CLIS)
libs: $(LIB_OBJS)
$(LIB_OBJS): %.o: %.cpp
	$(CXX) $(CXXFLAGS) -c $< -o $@
$(CLIS): %: %.o $(LIB_OBJS)
	$(CXX) $(CXXFLAGS) $^ -o $@
clean:
	rm -f $(CLIS) *.o
