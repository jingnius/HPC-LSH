#include "hpc_rss.h"

#include <cstdio>
#include <cstring>
#include <string>

std::size_t vmrss_kb() {
    FILE* fp = std::fopen("/proc/self/status", "r");
    if (fp == nullptr) {
        return 0;
    }
    char line[256];
    std::size_t kb = 0;
    while (std::fgets(line, sizeof(line), fp) != nullptr) {
        if (std::strncmp(line, "VmRSS:", 6) == 0) {
            std::sscanf(line + 6, "%zu", &kb);
            break;
        }
    }
    std::fclose(fp);
    return kb;
}

void touch_mapped_pages(const unsigned char* ptr, std::size_t len) {
    if (ptr == nullptr || len == 0) {
        return;
    }
    const std::size_t step = 4096;
    volatile unsigned char acc = 0;
    for (std::size_t off = 0; off < len; off += step) {
        acc ^= ptr[off];
    }
    (void)acc;
}

std::size_t rss_kb_for_mapped_file(const std::string& path) {
    FILE* fp = std::fopen("/proc/self/smaps", "r");
    if (fp == nullptr) {
        return 0;
    }
    char line[512];
    std::size_t total_kb = 0;
    bool in_region = false;
    while (std::fgets(line, sizeof(line), fp) != nullptr) {
        if (std::strstr(line, path.c_str()) != nullptr) {
            in_region = true;
            continue;
        }
        if (in_region) {
            if (line[0] >= '0' && line[0] <= '9') {
                in_region = false;
            } else if (std::strncmp(line, "Rss:", 4) == 0) {
                std::size_t kb = 0;
                std::sscanf(line + 4, "%zu", &kb);
                total_kb += kb;
            }
        }
    }
    std::fclose(fp);
    return total_kb;
}
