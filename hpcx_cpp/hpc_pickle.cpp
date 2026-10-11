#include "hpc_pickle.h"

#include <cstring>
#include <memory>
#include <stdexcept>

namespace {

using PicklePtr = std::shared_ptr<PickleValue>;

class PickleReader {
public:
    PickleReader(const unsigned char* data, std::size_t len) : data_(data), len_(len) {}

    bool load_root(PickleValue& out, std::string* err) {
        try {
            while (pos_ < len_) {
                if (!dispatch_one()) {
                    break;
                }
            }
            if (stack_.size() != 1 || !stack_.front()) {
                if (err) {
                    *err = "pickle stack size != 1 at end";
                }
                return false;
            }
            out = std::move(*stack_.front());
            stack_.clear();
            memo_.clear();
            return true;
        } catch (const std::exception& ex) {
            if (err) {
                *err = ex.what();
            }
            return false;
        }
    }

private:
    const unsigned char* data_;
    std::size_t len_;
    std::size_t pos_ = 0;
    std::vector<PicklePtr> stack_;
    std::vector<PicklePtr> memo_;

    void require(std::size_t n) {
        if (pos_ + n > len_) {
            throw std::runtime_error("pickle truncated");
        }
    }

    uint8_t read_u8() {
        require(1);
        return data_[pos_++];
    }

    uint16_t read_u16_le() {
        require(2);
        const uint16_t v = static_cast<uint16_t>(data_[pos_]) |
                           (static_cast<uint16_t>(data_[pos_ + 1]) << 8);
        pos_ += 2;
        return v;
    }

    int16_t read_i16_le() { return static_cast<int16_t>(read_u16_le()); }

    int32_t read_i32_le() {
        require(4);
        int32_t v = static_cast<int32_t>(
            static_cast<uint32_t>(data_[pos_]) |
            (static_cast<uint32_t>(data_[pos_ + 1]) << 8) |
            (static_cast<uint32_t>(data_[pos_ + 2]) << 16) |
            (static_cast<uint32_t>(data_[pos_ + 3]) << 24));
        pos_ += 4;
        return v;
    }

    int64_t read_i64_le() {
        require(8);
        int64_t v = 0;
        for (int i = 0; i < 8; ++i) {
            v |= static_cast<int64_t>(data_[pos_ + i]) << (8 * i);
        }
        pos_ += 8;
        return v;
    }

    std::string read_bytes(std::size_t n) {
        require(n);
        std::string s(reinterpret_cast<const char*>(data_ + pos_), n);
        pos_ += n;
        return s;
    }

    bool parse_frame() {
        const uint64_t frame_len = static_cast<uint64_t>(read_i64_le());
        const std::size_t frame_end = pos_ + static_cast<std::size_t>(frame_len);
        if (frame_end > len_) {
            throw std::runtime_error("pickle frame out of range");
        }
        while (pos_ < frame_end) {
            if (!dispatch_one()) {
                break;
            }
        }
        if (pos_ != frame_end) {
            throw std::runtime_error("pickle frame ended early");
        }
        return true;
    }

    PicklePtr pop_ptr() {
        if (stack_.empty()) {
            throw std::runtime_error("pickle pop empty stack");
        }
        PicklePtr v = stack_.back();
        stack_.pop_back();
        return v;
    }

    void push_ptr(PicklePtr v) { stack_.push_back(std::move(v)); }

    void push_val(PickleValue v) { push_ptr(std::make_shared<PickleValue>(std::move(v))); }

    PicklePtr& memo_get(std::size_t idx) {
        if (idx >= memo_.size() || !memo_[idx]) {
            throw std::runtime_error("pickle memo index out of range");
        }
        return memo_[idx];
    }

    bool dispatch_one() {
        if (pos_ >= len_) {
            return false;
        }
        const uint8_t op = read_u8();
        switch (op) {
        case 0x80: {  // PROTO
            read_u8();
            return true;
        }
        case 0x95: {  // FRAME
            return parse_frame();
        }
        case 0x5d: {  // EMPTY_LIST
            PickleValue l;
            l.kind = PickleValue::kList;
            l.list = std::make_unique<PickleList>();
            push_val(std::move(l));
            return true;
        }
        case 0x7d: {  // EMPTY_DICT
            PickleValue d;
            d.kind = PickleValue::kDict;
            d.dict = std::make_unique<PickleDict>();
            push_val(std::move(d));
            return true;
        }
        case 0x8f: {  // EMPTY_SET
            PickleValue s;
            s.kind = PickleValue::kSet;
            push_val(std::move(s));
            return true;
        }
        case 0x4e:
            push_val(PickleValue::null());
            return true;
        case 0x88:
            push_val(PickleValue::from_bool(true));
            return true;
        case 0x89:
            push_val(PickleValue::from_bool(false));
            return true;
        case 0x4a:
            push_val(PickleValue::from_int(read_i32_le()));
            return true;
        case 0x4b:
            push_val(PickleValue::from_int(read_u8()));
            return true;
        case 0x4d:  // BININT2 — treat as unsigned 16-bit (matches column-index APPENDS batches)
            push_val(PickleValue::from_int(static_cast<int64_t>(read_u16_le())));
            return true;
        case 0x8a: {  // LONG1
            const uint8_t n = read_u8();
            if (n == 0) {
                push_val(PickleValue::from_int(0));
                return true;
            }
            if (n <= 8) {
                int64_t v = 0;
                for (uint8_t i = 0; i < n; ++i) {
                    v |= static_cast<int64_t>(read_u8()) << (8 * i);
                }
                if (n < 8 && (v & (static_cast<int64_t>(1) << (8 * n - 1))) != 0) {
                    for (uint8_t i = n; i < 8; ++i) {
                        v |= static_cast<int64_t>(0xff) << (8 * i);
                    }
                }
                push_val(PickleValue::from_int(v));
            } else {
                PickleValue o;
                o.kind = PickleValue::kInt;
                o.wide = true;
                o.long_bytes.resize(n);
                unsigned __int128 v = 0;
                for (uint8_t i = 0; i < n; ++i) {
                    const uint8_t b = read_u8();
                    o.long_bytes[i] = b;
                    if (i < 16) {
                        v |= static_cast<unsigned __int128>(b) << (8 * i);
                    }
                }
                o.u = v;
                push_val(std::move(o));
            }
            return true;
        }
        case 0x8c: {  // SHORT_BINUNICODE
            const uint8_t n = read_u8();
            push_val(PickleValue::from_str(read_bytes(n)));
            return true;
        }
        case 0x94: {  // MEMOIZE
            if (stack_.empty()) {
                throw std::runtime_error("MEMOIZE empty stack");
            }
            memo_.push_back(stack_.back());
            return true;
        }
        case 0x68: {  // BINGET
            push_ptr(memo_get(read_u8()));
            return true;
        }
        case 0x79:
        case 0x6a: {  // LONG_BINGET
            push_ptr(memo_get(static_cast<std::size_t>(read_i32_le())));
            return true;
        }
        case 0x28: {  // MARK
            mark_stack_.push_back(stack_.size());
            return true;
        }
        case 0x65: {  // APPENDS
            if (mark_stack_.empty()) {
                throw std::runtime_error("APPENDS without mark");
            }
            const std::size_t k = mark_stack_.back();
            mark_stack_.pop_back();
            if (k > stack_.size() || stack_.empty()) {
                throw std::runtime_error("APPENDS bad mark");
            }
            std::vector<PicklePtr> items;
            items.reserve(stack_.size() - k);
            for (std::size_t i = k; i < stack_.size(); ++i) {
                items.push_back(stack_[i]);
            }
            stack_.resize(k);
            PickleValue& lst = *stack_.back();
            if (lst.kind != PickleValue::kList || !lst.list) {
                throw std::runtime_error("APPENDS target not list");
            }
            for (const PicklePtr& item : items) {
                lst.list->push_back(item);
            }
            return true;
        }
        case 0x75: {  // SETITEMS
            if (mark_stack_.empty()) {
                throw std::runtime_error("SETITEMS without mark");
            }
            const std::size_t k = mark_stack_.back();
            mark_stack_.pop_back();
            if (k > stack_.size() || stack_.empty()) {
                throw std::runtime_error("SETITEMS bad mark");
            }
            std::vector<PicklePtr> items;
            items.reserve(stack_.size() - k);
            for (std::size_t i = k; i < stack_.size(); ++i) {
                items.push_back(stack_[i]);
            }
            stack_.resize(k);
            PickleValue& dict = *stack_.back();
            if (dict.kind != PickleValue::kDict || !dict.dict) {
                throw std::runtime_error("SETITEMS target not dict");
            }
            if ((items.size() % 2) != 0) {
                throw std::runtime_error("SETITEMS odd items");
            }
            for (std::size_t i = 0; i < items.size(); i += 2) {
                const PickleValue& key = *items[i];
                std::string ks;
                if (key.kind == PickleValue::kInt) {
                    ks = std::to_string(key.i);
                } else if (key.kind == PickleValue::kStr) {
                    ks = key.s;
                } else {
                    throw std::runtime_error("SETITEMS bad key type");
                }
                dict.dict->upsert(ks) = items[i + 1];
            }
            return true;
        }
        case 0x90: {  // ADDITEMS
            if (mark_stack_.empty()) {
                throw std::runtime_error("ADDITEMS without mark");
            }
            const std::size_t k = mark_stack_.back();
            mark_stack_.pop_back();
            if (k > stack_.size() || stack_.empty()) {
                throw std::runtime_error("ADDITEMS bad mark");
            }
            std::vector<PicklePtr> items;
            items.reserve(stack_.size() - k);
            for (std::size_t i = k; i < stack_.size(); ++i) {
                items.push_back(stack_[i]);
            }
            stack_.resize(k);
            PickleValue& st = *stack_.back();
            if (st.kind != PickleValue::kSet) {
                throw std::runtime_error("ADDITEMS target not set");
            }
            for (const PicklePtr& item : items) {
                if (item->kind != PickleValue::kInt) {
                    throw std::runtime_error("ADDITEMS non-int");
                }
                st.set_ints.push_back(item->i);
            }
            return true;
        }
        case 0x6c:
        case 0x74: {  // LIST / TUPLE
            if (mark_stack_.empty()) {
                throw std::runtime_error("LIST/TUPLE without mark");
            }
            const std::size_t k = mark_stack_.back();
            mark_stack_.pop_back();
            if (k > stack_.size()) {
                throw std::runtime_error("LIST/TUPLE bad mark");
            }
            PickleValue lst;
            lst.kind = PickleValue::kList;
            lst.list = std::make_unique<PickleList>();
            for (std::size_t i = k; i < stack_.size(); ++i) {
                lst.list->push_back(stack_[i]);
            }
            stack_.resize(k);
            push_val(std::move(lst));
            return true;
        }
        case 0x61: {  // APPEND
            PicklePtr item = pop_ptr();
            PicklePtr lst = pop_ptr();
            if (!lst || lst->kind != PickleValue::kList || !lst->list) {
                throw std::runtime_error("APPEND target not list");
            }
            lst->list->push_back(item);
            push_ptr(std::move(lst));
            return true;
        }
        case 0x73: {  // SETITEM
            PicklePtr val = pop_ptr();
            PicklePtr key = pop_ptr();
            PicklePtr dict = pop_ptr();
            if (!dict || dict->kind != PickleValue::kDict || !dict->dict) {
                throw std::runtime_error("SETITEM target not dict");
            }
            std::string ks;
            if (key->kind == PickleValue::kInt) {
                ks = std::to_string(key->i);
            } else if (key->kind == PickleValue::kStr) {
                ks = key->s;
            } else {
                throw std::runtime_error("SETITEM bad key type");
            }
            dict->dict->upsert(ks) = val;
            push_ptr(std::move(dict));
            return true;
        }
        case 0x2e:
            return false;
        default:
            throw std::runtime_error(
                std::string("unsupported pickle opcode 0x") + std::to_string(static_cast<unsigned>(op)) +
                " at offset " + std::to_string(pos_ - 1));
        }
    }

    std::vector<std::size_t> mark_stack_;
};

}  // namespace

bool pickle_load_data_only(const unsigned char* data, std::size_t len, PickleValue& out, std::string* err) {
    PickleReader rd(data, len);
    return rd.load_root(out, err);
}
