#include "hpc_npz.h"

#include <cstring>
#include <stdexcept>
#include <zlib.h>

static bool inflate_raw(const unsigned char* in, std::size_t in_len, std::vector<unsigned char>& out) {
    z_stream strm{};
    if (inflateInit2(&strm, -MAX_WBITS) != Z_OK) {
        return false;
    }
    strm.next_in = const_cast<Bytef*>(in);
    strm.avail_in = static_cast<uInt>(in_len);
    unsigned char buf[65536];
    int rc = Z_OK;
    while (rc == Z_OK) {
        strm.next_out = buf;
        strm.avail_out = sizeof(buf);
        rc = inflate(&strm, Z_NO_FLUSH);
        if (rc != Z_OK && rc != Z_STREAM_END) {
            inflateEnd(&strm);
            return false;
        }
        const std::size_t have = sizeof(buf) - strm.avail_out;
        out.insert(out.end(), buf, buf + have);
    }
    inflateEnd(&strm);
    return rc == Z_STREAM_END;
}

static bool parse_npy(const unsigned char* data, std::size_t len, NpzArray& out) {
    if (len < 10 || data[0] != 0x93 || std::memcmp(data + 1, "NUMPY", 5) != 0) {
        return false;
    }
    const int ver = data[6] | (data[7] << 8);
    std::size_t header_len = 0;
    const unsigned char* header = nullptr;
    if (ver == 1) {
        if (len < 10) {
            return false;
        }
        header_len = static_cast<std::size_t>(data[8]) | (static_cast<std::size_t>(data[9]) << 8);
        header = data + 10;
    } else if (ver == 2) {
        if (len < 12) {
            return false;
        }
        header_len = static_cast<std::size_t>(data[8]) | (static_cast<std::size_t>(data[9]) << 8) |
                       (static_cast<std::size_t>(data[10]) << 16) |
                       (static_cast<std::size_t>(data[11]) << 24);
        header = data + 12;
    } else {
        return false;
    }
    const std::size_t header_off = static_cast<std::size_t>(header - data);
    if (header_off + header_len > len) {
        return false;
    }
    const std::string hdr(reinterpret_cast<const char*>(header), header_len);
    const auto descr_pos = hdr.find("'descr':");
    const auto shape_pos = hdr.find("'shape':");
    if (descr_pos == std::string::npos || shape_pos == std::string::npos) {
        return false;
    }
    const auto q1 = hdr.find('\'', descr_pos + 8);
    const auto q2 = hdr.find('\'', q1 + 1);
    out.descr = hdr.substr(q1 + 1, q2 - q1 - 1);
    out.shape.clear();
    const auto lp = hdr.find('(', shape_pos);
    const auto rp = hdr.find(')', lp);
    if (lp != std::string::npos && rp != std::string::npos) {
        std::string inside = hdr.substr(lp + 1, rp - lp - 1);
        std::size_t i = 0;
        while (i < inside.size()) {
            while (i < inside.size() && (inside[i] == ' ' || inside[i] == ',')) {
                ++i;
            }
            if (i >= inside.size()) {
                break;
            }
            std::size_t j = i;
            while (j < inside.size() && inside[j] != ',') {
                ++j;
            }
            const std::string tok = inside.substr(i, j - i);
            if (!tok.empty()) {
                out.shape.push_back(static_cast<std::size_t>(std::stoull(tok)));
            }
            i = j + 1;
        }
    }
    const std::size_t data_off = header_off + header_len;
    if (data_off > len) {
        return false;
    }
    out.data.assign(data + data_off, data + len);
    return true;
}

static bool deflate_raw(const unsigned char* in, std::size_t in_len, std::vector<unsigned char>& out) {
    z_stream strm{};
    if (deflateInit2(&strm, Z_DEFAULT_COMPRESSION, Z_DEFLATED, -MAX_WBITS, 8, Z_DEFAULT_STRATEGY) != Z_OK) {
        return false;
    }
    strm.next_in = const_cast<Bytef*>(in);
    strm.avail_in = static_cast<uInt>(in_len);
    unsigned char buf[65536];
    int rc = Z_OK;
    while (rc == Z_OK) {
        strm.next_out = buf;
        strm.avail_out = sizeof(buf);
        rc = deflate(&strm, Z_FINISH);
        if (rc != Z_OK && rc != Z_STREAM_END) {
            deflateEnd(&strm);
            return false;
        }
        const std::size_t have = sizeof(buf) - strm.avail_out;
        out.insert(out.end(), buf, buf + have);
    }
    deflateEnd(&strm);
    return rc == Z_STREAM_END;
}

static void append_u16(std::vector<unsigned char>& out, uint16_t v) {
    out.push_back(static_cast<unsigned char>(v & 0xff));
    out.push_back(static_cast<unsigned char>((v >> 8) & 0xff));
}

static void append_u32(std::vector<unsigned char>& out, uint32_t v) {
    out.push_back(static_cast<unsigned char>(v & 0xff));
    out.push_back(static_cast<unsigned char>((v >> 8) & 0xff));
    out.push_back(static_cast<unsigned char>((v >> 16) & 0xff));
    out.push_back(static_cast<unsigned char>((v >> 24) & 0xff));
}

static std::vector<unsigned char> build_npy(const NpzArray& arr) {
    std::string header = "{'descr': '" + arr.descr + "', 'fortran_order': False, 'shape': (";
    if (arr.shape.empty()) {
        header += ")";
    } else if (arr.shape.size() == 1) {
        header += std::to_string(arr.shape[0]) + ",)";
    } else {
        for (std::size_t i = 0; i < arr.shape.size(); ++i) {
            if (i > 0) {
                header += ", ";
            }
            header += std::to_string(arr.shape[i]);
        }
        header += ")";
    }
    header += ", }";
    const std::size_t preamble = 10;
    while ((preamble + header.size()) % 16 != 0) {
        header.push_back(' ');
    }
    header.push_back('\n');

    std::vector<unsigned char> out;
    out.reserve(preamble + header.size() + arr.data.size());
    out.push_back(static_cast<unsigned char>(0x93));
    out.insert(out.end(), {'N', 'U', 'M', 'P', 'Y'});
    out.push_back(1);
    out.push_back(0);
    const uint16_t header_len = static_cast<uint16_t>(header.size());
    append_u16(out, header_len);
    out.insert(out.end(), header.begin(), header.end());
    out.insert(out.end(), arr.data.begin(), arr.data.end());
    return out;
}

struct ZipEntryWrite {
    std::string name;
    std::vector<unsigned char> raw;
    std::vector<unsigned char> comp;
    uint32_t crc = 0;
};

static bool build_zip_entries(
    const std::unordered_map<std::string, NpzArray>& arrs,
    std::vector<ZipEntryWrite>& entries) {
    entries.clear();
    entries.reserve(arrs.size());
    for (const auto& kv : arrs) {
        ZipEntryWrite ent;
        ent.name = kv.first;
        ent.raw = build_npy(kv.second);
        ent.crc = static_cast<uint32_t>(::crc32(0, ent.raw.data(), static_cast<uInt>(ent.raw.size())));
        if (!deflate_raw(ent.raw.data(), ent.raw.size(), ent.comp)) {
            return false;
        }
        entries.push_back(std::move(ent));
    }
    return !entries.empty();
}

bool npz_save_compressed(
    const std::unordered_map<std::string, NpzArray>& arrs,
    std::vector<unsigned char>& out) {
    out.clear();
    std::vector<ZipEntryWrite> entries;
    if (!build_zip_entries(arrs, entries)) {
        return false;
    }

    struct ZipRec {
        std::size_t local_off = 0;
        const ZipEntryWrite* ent = nullptr;
    };
    std::vector<ZipRec> recs;
    recs.reserve(entries.size());

    for (const ZipEntryWrite& ent : entries) {
        ZipRec rec;
        rec.local_off = out.size();
        rec.ent = &ent;
        recs.push_back(rec);

        append_u32(out, 0x04034b50u);
        append_u16(out, 20);
        append_u16(out, 0);
        append_u16(out, 8);
        append_u16(out, 0);
        append_u16(out, 0);
        append_u32(out, ent.crc);
        append_u32(out, static_cast<uint32_t>(ent.comp.size()));
        append_u32(out, static_cast<uint32_t>(ent.raw.size()));
        append_u16(out, static_cast<uint16_t>(ent.name.size()));
        append_u16(out, 0);
        out.insert(out.end(), ent.name.begin(), ent.name.end());
        out.insert(out.end(), ent.comp.begin(), ent.comp.end());
    }

    const std::size_t central_off = out.size();
    for (const ZipRec& rec : recs) {
        const ZipEntryWrite& ent = *rec.ent;
        append_u32(out, 0x02014b50u);
        append_u16(out, 20);
        append_u16(out, 20);
        append_u16(out, 0);
        append_u16(out, 8);
        append_u16(out, 0);
        append_u16(out, 0);
        append_u32(out, ent.crc);
        append_u32(out, static_cast<uint32_t>(ent.comp.size()));
        append_u32(out, static_cast<uint32_t>(ent.raw.size()));
        append_u16(out, static_cast<uint16_t>(ent.name.size()));
        append_u16(out, 0);
        append_u16(out, 0);
        append_u16(out, 0);
        append_u16(out, 0);
        append_u32(out, 0);
        append_u32(out, static_cast<uint32_t>(rec.local_off));
        out.insert(out.end(), ent.name.begin(), ent.name.end());
    }

    const std::size_t central_size = out.size() - central_off;
    append_u32(out, 0x06054b50u);
    append_u16(out, 0);
    append_u16(out, 0);
    append_u16(out, static_cast<uint16_t>(recs.size()));
    append_u16(out, static_cast<uint16_t>(recs.size()));
    append_u32(out, static_cast<uint32_t>(central_size));
    append_u32(out, static_cast<uint32_t>(central_off));
    append_u16(out, 0);
    return true;
}

bool npz_load(const unsigned char* data, std::size_t len, std::unordered_map<std::string, NpzArray>& out) {
    out.clear();
    std::size_t pos = 0;
    while (pos + 30 <= len) {
        if (data[pos] != 0x50 || data[pos + 1] != 0x4b || data[pos + 2] != 0x03 || data[pos + 3] != 0x04) {
            break;
        }
        const uint16_t method = static_cast<uint16_t>(data[pos + 8]) | (static_cast<uint16_t>(data[pos + 9]) << 8);
        const uint32_t comp_size =
            static_cast<uint32_t>(data[pos + 18]) | (static_cast<uint32_t>(data[pos + 19]) << 8) |
            (static_cast<uint32_t>(data[pos + 20]) << 16) | (static_cast<uint32_t>(data[pos + 21]) << 24);
        const uint16_t name_len = static_cast<uint16_t>(data[pos + 26]) | (static_cast<uint16_t>(data[pos + 27]) << 8);
        const uint16_t extra_len = static_cast<uint16_t>(data[pos + 28]) | (static_cast<uint16_t>(data[pos + 29]) << 8);
        pos += 30;
        if (pos + name_len + extra_len + comp_size > len) {
            return false;
        }
        const std::string name(reinterpret_cast<const char*>(data + pos), name_len);
        pos += name_len + extra_len;
        const unsigned char* comp = data + pos;
        pos += comp_size;
        std::vector<unsigned char> raw;
        if (method == 0) {
            raw.assign(comp, comp + comp_size);
        } else if (method == 8) {
            if (!inflate_raw(comp, comp_size, raw)) {
                return false;
            }
        } else {
            return false;
        }
        NpzArray arr;
        if (!parse_npy(raw.data(), raw.size(), arr)) {
            return false;
        }
        out[name] = std::move(arr);
    }
    return !out.empty();
}
