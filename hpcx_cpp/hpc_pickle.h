#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

struct PickleValue;

using PicklePtr = std::shared_ptr<PickleValue>;
using PickleList = std::vector<PicklePtr>;

// Insertion-ordered dict (matches Python 3.7+ pickle SETITEMS order).
struct PickleDict {
    std::vector<std::pair<std::string, PicklePtr>> entries;
    std::unordered_map<std::string, std::size_t> index;

    PicklePtr& upsert(const std::string& key) {
        const auto it = index.find(key);
        if (it != index.end()) {
            return entries[it->second].second;
        }
        const std::size_t pos = entries.size();
        index[key] = pos;
        entries.emplace_back(key, nullptr);
        return entries.back().second;
    }

    const PicklePtr* get(const std::string& key) const {
        const auto it = index.find(key);
        if (it == index.end()) {
            return nullptr;
        }
        return &entries[it->second].second;
    }

    std::size_t size() const { return entries.size(); }
};

struct PickleValue {
    enum Kind { kNull, kBool, kInt, kStr, kList, kDict, kSet } kind = kNull;
    bool b = false;
    bool wide = false;
    int64_t i = 0;
    unsigned __int128 u = 0;
    std::vector<uint8_t> long_bytes;
    std::string s;
    std::unique_ptr<PickleList> list;
    std::unique_ptr<PickleDict> dict;
    std::vector<int64_t> set_ints;

    static PickleValue null() { return PickleValue{}; }
    static PickleValue from_bool(bool v) {
        PickleValue o;
        o.kind = kBool;
        o.b = v;
        return o;
    }
    static PickleValue from_int(int64_t v) {
        PickleValue o;
        o.kind = kInt;
        o.i = v;
        return o;
    }
    static PickleValue from_uint128(unsigned __int128 v) {
        PickleValue o;
        o.kind = kInt;
        o.wide = true;
        o.u = v;
        return o;
    }
    static PickleValue from_str(std::string v) {
        PickleValue o;
        o.kind = kStr;
        o.s = std::move(v);
        return o;
    }

    bool is_dict() const { return kind == kDict && dict != nullptr; }
    bool is_list() const { return kind == kList && list != nullptr; }

    const PickleDict* as_dict() const { return kind == kDict && dict ? dict.get() : nullptr; }
    const PickleList* as_list() const { return kind == kList && list ? list.get() : nullptr; }

    const PickleValue* dict_get(const std::string& key) const {
        if (kind != kDict || !dict) {
            return nullptr;
        }
        const PicklePtr* p = dict->get(key);
        return p != nullptr && *p ? p->get() : nullptr;
    }

    int64_t as_int64(int64_t def = 0) const { return kind == kInt ? i : def; }

    PickleValue() = default;
    PickleValue(const PickleValue&) = delete;
    PickleValue& operator=(const PickleValue&) = delete;
    PickleValue(PickleValue&&) noexcept = default;
    PickleValue& operator=(PickleValue&&) noexcept = default;
};

bool pickle_load_data_only(const unsigned char* data, std::size_t len, PickleValue& out, std::string* err);
