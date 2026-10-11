#pragma once

#include <cstddef>
#include <string>

// Process VmRSS from /proc/self/status (kB).
std::size_t vmrss_kb();

// Sum Rss (kB) for mmap regions backed by `path` from /proc/self/smaps.
std::size_t rss_kb_for_mapped_file(const std::string& path);

// Touch every page in [ptr, ptr+len) to fault mmap pages into RAM.
void touch_mapped_pages(const unsigned char* ptr, std::size_t len);
