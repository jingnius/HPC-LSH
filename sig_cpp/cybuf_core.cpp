#include "cybuf_core.h"

#include "inver_io.h"
#include "native_format.h"
#include "numpy_mt19937.h"

#include <algorithm>
#include <array>
#include <cstdlib>
#include <cstring>
#include <dirent.h>
#include <fcntl.h>
#include <fstream>
#include <queue>
#include <sys/mman.h>
#include <sys/stat.h>
#include <cerrno>
#include <cstdio>
#include <unistd.h>
#include <unordered_map>

CybufMmapRegion::~CybufMmapRegion() {
  if (base != nullptr && base != MAP_FAILED) {
    munmap(const_cast<uint8_t*>(base), size);
  }
  if (fd >= 0) {
    close(fd);
  }
}

void CybufAux::for_each_col_gram(uint32_t col_idx,
                                 const std::function<void(uint32_t gram_id)>& fn) const {
  if (col_idx >= n_cols) {
    return;
  }
  if (!col_grams.empty()) {
    for (uint32_t g : col_grams[col_idx]) {
      fn(g);
    }
    return;
  }
  if (flat_grams == nullptr || gram_off.size() < static_cast<std::size_t>(col_idx) + 2u) {
    return;
  }
  const uint64_t b = gram_off[col_idx];
  const uint64_t e = gram_off[col_idx + 1];
  for (uint64_t p = b; p < e; ++p) {
    fn(flat_grams[p]);
  }
}

namespace {

bool env_truthy(const char* name) {
  const char* v = std::getenv(name);
  if (v == nullptr || v[0] == '\0') {
    return false;
  }
  return v[0] != '0';
}

/** Default: mmap CYBF. Opt-out: SIG_CYBUF_MMAP=0 or SIG_CYBUF_VECTOR=1. */
bool cybuf_read_use_mmap() {
  if (const char* explicit_mmap = std::getenv("SIG_CYBUF_MMAP"); explicit_mmap != nullptr && explicit_mmap[0] != '\0') {
    return explicit_mmap[0] != '0';
  }
  if (env_truthy("SIG_CYBUF_VECTOR")) {
    return false;
  }
  return true;
}

bool cybuf_read_native_mmap(const std::string& path, CybufAux* out, std::string* err) {
  int fd = open(path.c_str(), O_RDONLY);
  if (fd < 0) {
    if (err) {
      *err = "cannot open cybuf: " + path;
    }
    return false;
  }
  struct stat st {};
  if (fstat(fd, &st) != 0 || st.st_size < static_cast<off_t>(sizeof(NativeCybufHeader))) {
    close(fd);
    if (err) {
      *err = "bad cybuf file size";
    }
    return false;
  }
  const size_t file_size = static_cast<size_t>(st.st_size);
  void* mapped = mmap(nullptr, file_size, PROT_READ, MAP_PRIVATE, fd, 0);
  if (mapped == MAP_FAILED) {
    close(fd);
    if (err) {
      *err = "mmap cybuf failed: " + path;
    }
    return false;
  }
  auto region = std::make_shared<CybufMmapRegion>();
  region->fd = fd;
  region->size = file_size;
  region->base = static_cast<const uint8_t*>(mapped);

  const auto* base = region->base;
  NativeCybufHeader hdr{};
  std::memcpy(&hdr, base, sizeof(hdr));
  if (hdr.magic != NATIVE_CYBF_MAGIC || hdr.version != NATIVE_FORMAT_VERSION) {
    if (err) {
      *err = "bad CYBF magic/version";
    }
    return false;
  }

  out->mmap = std::move(region);
  out->n_cols = hdr.n_cols;
  out->max_gram_id = hdr.max_gram_id;
  out->num_perm = hdr.num_perm;
  out->col_grams.clear();

  size_t off = sizeof(hdr);
  out->col_ids.resize(hdr.n_cols);
  if (hdr.n_cols > 0) {
    std::memcpy(out->col_ids.data(), base + off, hdr.n_cols * sizeof(uint64_t));
    off += hdr.n_cols * sizeof(uint64_t);
  }
  out->gram_off.resize(static_cast<std::size_t>(hdr.n_cols) + 1u);
  if (hdr.n_cols > 0) {
    std::memcpy(out->gram_off.data(), base + off, (hdr.n_cols + 1) * sizeof(uint64_t));
    off += (hdr.n_cols + 1) * sizeof(uint64_t);
  }
  out->flat_grams = reinterpret_cast<const uint32_t*>(base + off);
  off += static_cast<size_t>(hdr.n_gram_entries) * sizeof(uint32_t);

  out->gram_counts.assign(static_cast<std::size_t>(hdr.max_gram_id) + 1u, 0);
  if (!out->gram_counts.empty()) {
    std::memcpy(out->gram_counts.data(), base + off, out->gram_counts.size() * sizeof(uint32_t));
  }
  return true;
}

bool read_exact(std::ifstream& in, void* buf, std::size_t n, std::string* err) {
    in.read(static_cast<char*>(buf), static_cast<std::streamsize>(n));
    if (!in) {
        if (err) {
            *err = "unexpected EOF";
        }
        return false;
    }
    return true;
}

struct MutableInver {
    std::vector<std::vector<uint32_t>> by_id;
    int max_id = 0;
};

MutableInver inver_from_csr(const DenseInver& csr) {
    MutableInver out;
    out.max_id = static_cast<int>(csr.max_gram_id);
    out.by_id.resize(static_cast<std::size_t>(out.max_id) + 1u);
    const uint64_t* off = inver_offsets_cptr(csr);
    const uint32_t* cols = inver_col_ids_cptr(csr);
    for (uint32_t gid = 0; gid <= csr.max_gram_id; ++gid) {
        const uint64_t b = off[gid];
        const uint64_t e = off[gid + 1];
        out.by_id[gid].assign(cols + static_cast<std::size_t>(b), cols + static_cast<std::size_t>(e));
    }
    return out;
}

/** Lazy gram lists: materialize only sampled/edited posting lists (CSR-backed). */
struct LazyInver {
    DenseInver csr;
    std::unordered_map<uint32_t, std::vector<uint32_t>> hot;

    std::vector<uint32_t>& gram(int sample_id) {
        const uint32_t gid = static_cast<uint32_t>(sample_id);
        auto it = hot.find(gid);
        if (it != hot.end()) {
            return it->second;
        }
        auto& vec = hot[gid];
        const uint64_t* off = inver_offsets_cptr(csr);
        const uint32_t* cols = inver_col_ids_cptr(csr);
        if (sample_id >= 0 && sample_id <= static_cast<int>(csr.max_gram_id) && off != nullptr && cols != nullptr) {
            const uint64_t b = off[gid];
            const uint64_t e = off[gid + 1];
            vec.assign(cols + static_cast<std::size_t>(b), cols + static_cast<std::size_t>(e));
        }
        return vec;
    }
};

bool sig_lazy_inver_enabled() {
    if (const char* v = std::getenv("SIG_LAZY_INVER")) {
        return v[0] != '0';
    }
    return true;
}

bool sig_stream_columns_enabled() {
    if (const char* v = std::getenv("SIG_STREAM_COLUMNS")) {
        return v[0] != '0';
    }
    return true;
}


uint32_t sig_stream_batch_size() {
    if (const char* v = std::getenv("SIG_STREAM_BATCH_SIZE")) {
        const unsigned long x = std::strtoul(v, nullptr, 10);
        if (x > 0ul) {
            return static_cast<uint32_t>(x);
        }
    }
    return 100000u;
}

bool sig_inver_mmap_enabled() {
    if (const char* v = std::getenv("SIG_INVER_MMAP")) {
        return v[0] != '0';
    }
    return true;
}

bool write_sig_column_record(std::ostream& out, const ColumnSig& col, uint64_t* n_sig_ints_acc) {
    out.write(reinterpret_cast<const char*>(&col.col_id), sizeof(col.col_id));
    out.write(reinterpret_cast<const char*>(&col.hit_count), sizeof(col.hit_count));
    out.write(reinterpret_cast<const char*>(&col.sig_len), sizeof(col.sig_len));
    const uint32_t raw_len = static_cast<uint32_t>(col.sig.size());
    out.write(reinterpret_cast<const char*>(&raw_len), sizeof(raw_len));
    if (raw_len > 0) {
        out.write(reinterpret_cast<const char*>(col.sig.data()),
                  static_cast<std::streamsize>(raw_len * sizeof(int32_t)));
    }
    if (n_sig_ints_acc) {
        *n_sig_ints_acc += raw_len;
    }
    return static_cast<bool>(out);
}

class SigBandStreamWriter {
  public:
    bool open(const std::string& path, uint32_t band, uint32_t num_perm, uint32_t n_cols_expected, std::string* err) {
        path_ = path;
        parts_dir_ = path_ + ".stream_parts";
        band_ = band;
        num_perm_ = num_perm;
        n_cols_expected_ = n_cols_expected;
        n_sig_ints_ = 0;
        n_written_ = 0;
        batch_seq_ = 0;
        batch_size_ = sig_stream_batch_size();
        pending_.clear();
        batch_paths_.clear();
        written_.assign(n_cols_expected_, 0);
        if (mkdir(parts_dir_.c_str(), 0755) != 0 && errno != EEXIST) {
            if (err) {
                *err = "cannot create sig stream parts dir: " + parts_dir_;
            }
            return false;
        }
        clear_parts_dir();
        return true;
    }

    bool flush_pending(std::string* err) {
        if (pending_.empty()) {
            return true;
        }
        std::vector<uint32_t> keys;
        keys.reserve(pending_.size());
        for (const auto& kv : pending_) {
            keys.push_back(kv.first);
        }
        std::sort(keys.begin(), keys.end());

        char name[64];
        std::snprintf(name, sizeof(name), "batch_%06u.bin", batch_seq_);
        const std::string part_path = parts_dir_ + "/" + name;
        std::ofstream part(part_path, std::ios::binary | std::ios::trunc);
        if (!part) {
            if (err) {
                *err = "cannot write sig batch: " + part_path;
            }
            return false;
        }
        const uint32_t n = static_cast<uint32_t>(keys.size());
        part.write(reinterpret_cast<const char*>(&n), sizeof(n));
        for (uint32_t dense_idx : keys) {
            auto it = pending_.find(dense_idx);
            if (it == pending_.end()) {
                continue;
            }
            ColumnSig col = std::move(it->second);
            pending_.erase(it);
            part.write(reinterpret_cast<const char*>(&dense_idx), sizeof(dense_idx));
            uint64_t part_ints = 0;
            if (!write_sig_column_record(part, col, &part_ints)) {
                if (err) {
                    *err = "sig batch write failed: " + part_path;
                }
                return false;
            }
            n_sig_ints_ += part_ints;
            written_[dense_idx] = 1;
            ++n_written_;
        }
        if (!part) {
            if (err) {
                *err = "sig batch write failed: " + part_path;
            }
            return false;
        }
        pending_.clear();
        pending_.rehash(0);
        batch_paths_.push_back(part_path);
        ++batch_seq_;
        return true;
    }

    bool append_dense(uint32_t dense_idx, ColumnSig col, std::string* err) {
        if (dense_idx >= n_cols_expected_) {
            if (err) {
                *err = "sig stream dense index out of range";
            }
            return false;
        }
        if (written_[dense_idx] || pending_.count(dense_idx) > 0) {
            if (err) {
                *err = "duplicate sig stream column";
            }
            return false;
        }
        pending_[dense_idx] = std::move(col);
        if (pending_.size() >= batch_size_) {
            return flush_pending(err);
        }
        return true;
    }

    bool finalize(std::string* err) {
        if (!flush_pending(err)) {
            return false;
        }
        if (n_written_ != n_cols_expected_) {
            if (err) {
                *err = "sig stream column count mismatch";
            }
            return false;
        }
        written_.clear();
        written_.shrink_to_fit();
        pending_.clear();
        pending_.rehash(0);
        return merge_batches(err);
    }

    /** Merge only: caller must have released dictionary, inver, cybuf, and sig compute state. */
    bool merge_parts_from_disk(std::string* err) {
        if (!flush_pending(err)) {
            return false;
        }
        return finalize(err);
    }

  private:
    struct BatchHead {
        uint32_t dense_idx = 0;
        uint32_t file_i = 0;
        ColumnSig col;
        bool operator>(const BatchHead& other) const { return dense_idx > other.dense_idx; }
    };

    void clear_parts_dir() {
        DIR* dir = opendir(parts_dir_.c_str());
        if (dir == nullptr) {
            return;
        }
        while (dirent* ent = readdir(dir)) {
            if (ent->d_name[0] == '.') {
                continue;
            }
            const std::string part_path = parts_dir_ + "/" + ent->d_name;
            std::remove(part_path.c_str());
        }
        closedir(dir);
    }

    bool read_batch_record(std::ifstream& in, uint32_t* dense_idx, ColumnSig* col, std::string* err) {
        if (!read_exact(in, dense_idx, sizeof(*dense_idx), err)) {
            return false;
        }
        if (!read_exact(in, &col->col_id, sizeof(col->col_id), err)) {
            return false;
        }
        if (!read_exact(in, &col->hit_count, sizeof(col->hit_count), err)) {
            return false;
        }
        if (!read_exact(in, &col->sig_len, sizeof(col->sig_len), err)) {
            return false;
        }
        uint32_t raw_len = 0;
        if (!read_exact(in, &raw_len, sizeof(raw_len), err)) {
            return false;
        }
        col->sig.resize(raw_len);
        if (raw_len > 0 && !read_exact(in, col->sig.data(), raw_len * sizeof(int32_t), err)) {
            return false;
        }
        return true;
    }

    bool merge_batches(std::string* err) {
        std::ofstream out(path_, std::ios::binary | std::ios::trunc);
        if (!out) {
            if (err) {
                *err = "cannot open sig stream output: " + path_;
            }
            return false;
        }
        NativeSigBandHeader hdr{};
        hdr.magic = NATIVE_SIGB_MAGIC;
        hdr.version = NATIVE_FORMAT_VERSION;
        hdr.band = band_;
        hdr.num_perm = num_perm_;
        hdr.n_cols = n_cols_expected_;
        hdr.n_sig_ints = static_cast<uint64_t>(n_sig_ints_);
        out.write(reinterpret_cast<const char*>(&hdr), sizeof(hdr));

        std::vector<std::ifstream> inputs(batch_paths_.size());
        std::vector<uint32_t> left(batch_paths_.size(), 0);
        std::priority_queue<BatchHead, std::vector<BatchHead>, std::greater<BatchHead>> heap;
        for (uint32_t i = 0; i < batch_paths_.size(); ++i) {
            inputs[i].open(batch_paths_[i], std::ios::binary);
            if (!inputs[i]) {
                if (err) {
                    *err = "cannot open sig batch: " + batch_paths_[i];
                }
                return false;
            }
            if (!read_exact(inputs[i], &left[i], sizeof(left[i]), err)) {
                return false;
            }
            if (left[i] == 0) {
                continue;
            }
            BatchHead head;
            head.file_i = i;
            if (!read_batch_record(inputs[i], &head.dense_idx, &head.col, err)) {
                return false;
            }
            --left[i];
            heap.push(std::move(head));
        }

        uint32_t expect = 0;
        while (!heap.empty()) {
            BatchHead head = heap.top();
            heap.pop();
            if (head.dense_idx != expect) {
                if (err) {
                    *err = "sig batch merge is not dense-ordered";
                }
                return false;
            }
            if (!write_sig_column_record(out, head.col, nullptr)) {
                if (err) {
                    *err = "sig merge write failed";
                }
                return false;
            }
            ++expect;
            const uint32_t file_i = head.file_i;
            if (left[file_i] > 0) {
                BatchHead next;
                next.file_i = file_i;
                if (!read_batch_record(inputs[file_i], &next.dense_idx, &next.col, err)) {
                    return false;
                }
                --left[file_i];
                heap.push(std::move(next));
            }
        }
        if (expect != n_cols_expected_ || !out) {
            if (err) {
                *err = "sig batch merge column count mismatch";
            }
            return false;
        }
        for (std::ifstream& in : inputs) {
            in.close();
        }
        for (const std::string& part_path : batch_paths_) {
            std::remove(part_path.c_str());
        }
        batch_paths_.clear();
        rmdir(parts_dir_.c_str());
        return true;
    }

    std::string path_;
    std::string parts_dir_;
    uint32_t band_ = 0;
    uint32_t num_perm_ = 5;
    uint32_t n_cols_expected_ = 0;
    uint64_t n_sig_ints_ = 0;
    uint32_t n_written_ = 0;
    uint32_t batch_seq_ = 0;
    uint32_t batch_size_ = 100000;
    std::unordered_map<uint32_t, ColumnSig> pending_;
    std::vector<uint8_t> written_;
    std::vector<std::string> batch_paths_;
};

struct PoolState {
    std::vector<int> pool_list;
    std::vector<int> pool_pos;
    int max_id = 0;

    explicit PoolState(int max_id_in) : pool_pos(static_cast<std::size_t>(max_id_in) + 1u, -1), max_id(max_id_in) {}

    void init_from_counts(const std::vector<uint32_t>& counts) {
        pool_list.clear();
        for (int gid = 0; gid <= max_id; ++gid) {
            if (gid < static_cast<int>(counts.size()) && counts[static_cast<std::size_t>(gid)] > 0) {
                pool_pos[static_cast<std::size_t>(gid)] = static_cast<int>(pool_list.size());
                pool_list.push_back(gid);
            }
        }
    }

    int size() const { return static_cast<int>(pool_list.size()); }

    int get_at(int i) const { return pool_list[static_cast<std::size_t>(i)]; }

    void remove_swap(int removed_idx) {
        if (removed_idx < 0 || removed_idx > max_id) {
            return;
        }
        const int i = pool_pos[static_cast<std::size_t>(removed_idx)];
        if (i < 0) {
            return;
        }
        const int last_i = static_cast<int>(pool_list.size()) - 1;
        const int last_val = pool_list[static_cast<std::size_t>(last_i)];
        pool_list[static_cast<std::size_t>(i)] = last_val;
        pool_pos[static_cast<std::size_t>(last_val)] = i;
        pool_list.pop_back();
        pool_pos[static_cast<std::size_t>(removed_idx)] = -1;
    }
};

struct GramState {
    std::vector<int> counts;
    int max_id = 0;

    explicit GramState(int max_id_in) : counts(static_cast<std::size_t>(max_id_in) + 1u, 0), max_id(max_id_in) {}

    void decrement(int gidx, PoolState* pool) {
        if (gidx < 0 || gidx > max_id) {
            return;
        }
        int c = counts[static_cast<std::size_t>(gidx)];
        if (c <= 0) {
            return;
        }
        c -= 1;
        counts[static_cast<std::size_t>(gidx)] = c;
        if (c == 0) {
            pool->remove_swap(gidx);
        }
    }
};

struct SigState {
    const CybufAux* aux = nullptr;
    int n = 0;
    int num_perm = 5;
    std::vector<std::vector<int32_t>> sigs;
    std::vector<int> hit_counts;
    std::vector<int> sig_lens;
    std::vector<uint8_t> active;

    explicit SigState(const CybufAux& cy)
        : aux(&cy),
          n(static_cast<int>(cy.col_ids.size())),
          num_perm(static_cast<int>(cy.num_perm)),
          sigs(static_cast<std::size_t>(cy.col_ids.size())),
          hit_counts(static_cast<std::size_t>(cy.col_ids.size()), 0),
          sig_lens(static_cast<std::size_t>(cy.col_ids.size()), 0),
          active(static_cast<std::size_t>(cy.col_ids.size()), 1) {}
};

void mark_completed(SigState* st, int d) {
    st->active[static_cast<std::size_t>(d)] = 0;
}

void remove_one_col(int d, SigState* st, PoolState* pool, GramState* gram) {
    st->aux->for_each_col_gram(static_cast<uint32_t>(d), [&](uint32_t gidx) {
        gram->decrement(static_cast<int>(gidx), pool);
    });
}

void detach_completed_column(int d, int hc, SigState* st, std::vector<std::pair<int, ColumnSig>>* completed) {
    mark_completed(st, d);
    ColumnSig col;
    col.hit_count = static_cast<uint32_t>(hc);
    col.sig_len = static_cast<uint32_t>(st->sig_lens[static_cast<std::size_t>(d)]);
    col.sig = std::move(st->sigs[static_cast<std::size_t>(d)]);
    st->sigs[static_cast<std::size_t>(d)].clear();
    st->sigs[static_cast<std::size_t>(d)].shrink_to_fit();
    completed->push_back({d, std::move(col)});
}

void sample_step_fused_cpp_flat(
    int sample_id,
    LazyInver* inver,
    int signature_len,
    SigState* st,
    PoolState* pool,
    GramState* gram,
    std::vector<std::pair<int, ColumnSig>>* completed) {
    if (sample_id < 0 || sample_id > static_cast<int>(inver->csr.max_gram_id)) {
        return;
    }
    std::vector<uint32_t>& hit_vec = inver->gram(sample_id);
    const std::size_t n = hit_vec.size();
    std::size_t w = 0;
    for (std::size_t i = 0; i < n; ++i) {
        const int d = static_cast<int>(hit_vec[i]);
        if (d < 0 || d >= st->n || st->active[static_cast<std::size_t>(d)] == 0) {
            continue;
        }
        if (w != i) {
            hit_vec[w] = hit_vec[i];
        }
        ++w;

        std::vector<int32_t>& buf = st->sigs[static_cast<std::size_t>(d)];
        int slen = st->sig_lens[static_cast<std::size_t>(d)];
        if (slen + 1 < signature_len) {
            const int insert_0 = signature_len - (slen + 1);
            buf.push_back(-insert_0);
            slen += insert_0;
            st->sig_lens[static_cast<std::size_t>(d)] = slen;
        }
        buf.push_back(sample_id);
        const int hc = st->hit_counts[static_cast<std::size_t>(d)] + 1;
        st->hit_counts[static_cast<std::size_t>(d)] = hc;
        st->sig_lens[static_cast<std::size_t>(d)] = slen + 1;
        if (hc >= st->num_perm) {
            detach_completed_column(d, hc, st, completed);
            remove_one_col(d, st, pool, gram);
        }
    }
    if (w < n) {
        hit_vec.resize(w);
    }
}

DenseInver build_initial_inver(const CybufAux& aux) {
    DenseInver initial{};
    initial.max_gram_id = aux.max_gram_id;
    initial.offsets.assign(static_cast<std::size_t>(aux.max_gram_id) + 2u, 0);
    uint64_t off = 0;
    std::vector<std::vector<uint32_t>> lists(static_cast<std::size_t>(aux.max_gram_id) + 1u);
    for (uint32_t col_idx = 0; col_idx < aux.n_cols; ++col_idx) {
        aux.for_each_col_gram(col_idx, [&](uint32_t gid) { lists[gid].push_back(col_idx); });
    }
    for (uint32_t gid = 0; gid <= aux.max_gram_id; ++gid) {
        initial.offsets[gid] = off;
        for (uint32_t c : lists[gid]) {
            initial.col_ids.push_back(c);
            ++off;
        }
    }
    initial.offsets[aux.max_gram_id + 1] = off;
    return initial;
}

void sample_step_fused_cpp_flat_legacy(
    int sample_id,
    MutableInver* inver,
    int signature_len,
    SigState* st,
    PoolState* pool,
    GramState* gram,
    std::vector<std::pair<int, ColumnSig>>* completed) {
    if (sample_id < 0 || sample_id > inver->max_id) {
        return;
    }
    std::vector<uint32_t>& hit_vec = inver->by_id[static_cast<std::size_t>(sample_id)];
    const std::size_t n = hit_vec.size();
    std::size_t w = 0;
    for (std::size_t i = 0; i < n; ++i) {
        const int d = static_cast<int>(hit_vec[i]);
        if (d < 0 || d >= st->n || st->active[static_cast<std::size_t>(d)] == 0) {
            continue;
        }
        if (w != i) {
            hit_vec[w] = hit_vec[i];
        }
        ++w;

        std::vector<int32_t>& buf = st->sigs[static_cast<std::size_t>(d)];
        int slen = st->sig_lens[static_cast<std::size_t>(d)];
        if (slen + 1 < signature_len) {
            const int insert_0 = signature_len - (slen + 1);
            buf.push_back(-insert_0);
            slen += insert_0;
            st->sig_lens[static_cast<std::size_t>(d)] = slen;
        }
        buf.push_back(sample_id);
        const int hc = st->hit_counts[static_cast<std::size_t>(d)] + 1;
        st->hit_counts[static_cast<std::size_t>(d)] = hc;
        st->sig_lens[static_cast<std::size_t>(d)] = slen + 1;
        if (hc >= st->num_perm) {
            detach_completed_column(d, hc, st, completed);
            remove_one_col(d, st, pool, gram);
        }
    }
    if (w < n) {
        hit_vec.resize(w);
    }
}

static void release_lazy_inver(LazyInver* inv) {
    if (inv == nullptr) {
        return;
    }
    inv->hot.clear();
    inv->hot.rehash(0);
    if (inv->csr.mmap) {
        inv->csr.mmap.reset();
        inv->csr.offsets_view = nullptr;
        inv->csr.col_ids_view = nullptr;
    } else {
        inv->csr.offsets.clear();
        inv->csr.offsets.shrink_to_fit();
        inv->csr.col_ids.clear();
        inv->csr.col_ids.shrink_to_fit();
    }
    inv->csr.max_gram_id = 0;
}

static void release_legacy_inver(MutableInver* inv) {
    if (inv == nullptr) {
        return;
    }
    inv->by_id.clear();
    inv->by_id.shrink_to_fit();
    inv->max_id = 0;
}

static void release_sig_state(SigState* st) {
    if (st == nullptr) {
        return;
    }
    st->sigs.clear();
    st->sigs.shrink_to_fit();
    st->hit_counts.clear();
    st->hit_counts.shrink_to_fit();
    st->sig_lens.clear();
    st->sig_lens.shrink_to_fit();
    st->active.clear();
    st->active.shrink_to_fit();
    st->aux = nullptr;
}

bool sig_build_impl(CybufAux& aux, const std::vector<int32_t>* sample_order, int seed, uint32_t band,
                    SigBand* out, std::vector<int32_t>* sample_order_out, const SigBuildOptions* opts,
                    std::string* err) {
    if (!out) {
        if (err) {
            *err = "null out";
        }
        return false;
    }

    const bool use_lazy = sig_lazy_inver_enabled();
    const char* stream_path =
        (opts != nullptr && opts->stream_out_path != nullptr && opts->stream_out_path[0] != '\0')
            ? opts->stream_out_path
            : nullptr;
    const bool use_stream = stream_path != nullptr && sig_stream_columns_enabled();

    const uint32_t n_cols_expected = aux.n_cols;
    const uint32_t num_perm = aux.num_perm;

    SigBandStreamWriter stream_writer;
    SigBandStreamWriter* stream = nullptr;
    if (use_stream) {
        if (!stream_writer.open(stream_path, band, num_perm, n_cols_expected, err)) {
            return false;
        }
        stream = &stream_writer;
    }

    std::vector<int32_t> generated_order;
    if (sample_order) {
        generated_order = *sample_order;
    }

    {
        LazyInver lazy_inver;
        MutableInver legacy_inver;
        const bool use_legacy_inver = !use_lazy;

        if (use_lazy) {
            const char* inver_path = (opts != nullptr && opts->inver_path != nullptr) ? opts->inver_path : nullptr;
            bool loaded_inver = false;
            if (inver_path != nullptr && inver_path[0] != '\0') {
                struct stat st {};
                if (stat(inver_path, &st) == 0 && st.st_size > 0) {
                    if (sig_inver_mmap_enabled()) {
                        loaded_inver = inver_read_native_mmap(inver_path, &lazy_inver.csr, err);
                    } else {
                        loaded_inver = inver_read_native(inver_path, &lazy_inver.csr, err);
                    }
                    if (!loaded_inver) {
                        return false;
                    }
                }
            }
            if (!loaded_inver) {
                lazy_inver.csr = build_initial_inver(aux);
            }
        } else {
            const DenseInver initial = build_initial_inver(aux);
            legacy_inver = inver_from_csr(initial);
        }

        PoolState pool(static_cast<int>(aux.max_gram_id));
        pool.init_from_counts(aux.gram_counts);
        GramState gram(static_cast<int>(aux.max_gram_id));
        for (std::size_t i = 0; i < aux.gram_counts.size(); ++i) {
            gram.counts[i] = static_cast<int>(aux.gram_counts[i]);
        }
        SigState st(aux);

        std::vector<ColumnSig> done_by_dense;
        if (!use_stream) {
            done_by_dense.resize(aux.col_ids.size());
        }
        std::vector<uint8_t> have_done(aux.col_ids.size(), 0);
        numpy_mt19937::NumpyMtRng rng(seed);

        int signature_len = 0;
        int completed = 0;
        const int dataset_len = static_cast<int>(aux.col_ids.size());
        std::size_t order_idx = 0;

        while (completed < dataset_len) {
        const int pool_n = pool.size();
        if (pool_n == 0) {
            break;
        }
        signature_len += 1;

        int sample_id = 0;
        if (sample_order) {
            if (order_idx >= sample_order->size()) {
                break;
            }
            sample_id = static_cast<int>((*sample_order)[order_idx++]);
        } else {
            const int ri = rng.randint0(pool_n);
            sample_id = pool.get_at(ri);
            generated_order.push_back(static_cast<int32_t>(sample_id));
        }

        std::vector<std::pair<int, ColumnSig>> newly_done;
        if (use_legacy_inver) {
            sample_step_fused_cpp_flat_legacy(sample_id, &legacy_inver, signature_len, &st, &pool, &gram, &newly_done);
        } else {
            sample_step_fused_cpp_flat(sample_id, &lazy_inver, signature_len, &st, &pool, &gram, &newly_done);
        }
        for (auto& item : newly_done) {
            const int d = item.first;
            item.second.col_id = aux.col_ids[static_cast<std::size_t>(d)];
            have_done[static_cast<std::size_t>(d)] = 1;
            completed += 1;
            if (stream != nullptr) {
                if (!stream->append_dense(static_cast<uint32_t>(d), std::move(item.second), err)) {
                    return false;
                }
            } else {
                done_by_dense[static_cast<std::size_t>(d)] = std::move(item.second);
            }
        }
        }

        for (uint32_t d = 0; d < aux.col_ids.size(); ++d) {
            if (!have_done[d]) {
                if (err) {
                    *err = "missing completed column in build";
                }
                return false;
            }
        }

        out->band = band;
        out->num_perm = num_perm;
        out->n_cols = n_cols_expected;
        out->n_samples = static_cast<uint32_t>(sample_order ? sample_order->size() : generated_order.size());
        out->columns.clear();

        if (stream != nullptr) {
            release_sig_state(&st);
            if (use_legacy_inver) {
                release_legacy_inver(&legacy_inver);
            } else {
                release_lazy_inver(&lazy_inver);
            }
            cybuf_release_build_artifacts(&aux);
        } else {
            out->columns.reserve(aux.col_ids.size());
            for (uint32_t d = 0; d < aux.col_ids.size(); ++d) {
                out->columns.push_back(done_by_dense[d]);
            }
        }
    }

    if (stream != nullptr) {
        if (!stream->merge_parts_from_disk(err)) {
            return false;
        }
    }

    if (sample_order_out) {
        *sample_order_out = std::move(generated_order);
    }
    return true;
}

}  // namespace

void cybuf_release_build_artifacts(CybufAux* cy) {
    if (cy == nullptr) {
        return;
    }
    cy->col_grams.clear();
    cy->col_grams.shrink_to_fit();
    cy->gram_counts.clear();
    cy->gram_counts.shrink_to_fit();
    cy->col_ids.clear();
    cy->col_ids.shrink_to_fit();
    cy->gram_off.clear();
    cy->gram_off.shrink_to_fit();
    cy->flat_grams = nullptr;
    cy->mmap.reset();
    cy->n_cols = 0;
    cy->max_gram_id = 0;
}

bool cybuf_read_native(const std::string& path, CybufAux* out, std::string* err) {
    if (!out) {
        if (err) {
            *err = "null out";
        }
        return false;
    }
    out->mmap.reset();
    out->flat_grams = nullptr;
    out->gram_off.clear();
    if (cybuf_read_use_mmap()) {
        return cybuf_read_native_mmap(path, out, err);
    }
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        if (err) {
            *err = "cannot open cybuf: " + path;
        }
        return false;
    }
    NativeCybufHeader hdr{};
    if (!read_exact(in, &hdr, sizeof(hdr), err)) {
        return false;
    }
    if (hdr.magic != NATIVE_CYBF_MAGIC || hdr.version != NATIVE_FORMAT_VERSION) {
        if (err) {
            *err = "bad CYBF magic/version";
        }
        return false;
    }
    out->max_gram_id = hdr.max_gram_id;
    out->num_perm = hdr.num_perm;
    out->col_ids.resize(hdr.n_cols);
    if (hdr.n_cols > 0 && !read_exact(in, out->col_ids.data(), hdr.n_cols * sizeof(uint64_t), err)) {
        return false;
    }
    std::vector<uint64_t> gram_off(hdr.n_cols + 1);
    if (hdr.n_cols > 0 &&
        !read_exact(in, gram_off.data(), (hdr.n_cols + 1) * sizeof(uint64_t), err)) {
        return false;
    }
    out->col_grams.assign(hdr.n_cols, {});
    if (hdr.n_gram_entries > 0) {
        std::vector<uint32_t> grams(hdr.n_gram_entries);
        if (!read_exact(in, grams.data(), grams.size() * sizeof(uint32_t), err)) {
            return false;
        }
        for (uint32_t ci = 0; ci < hdr.n_cols; ++ci) {
            const auto b = static_cast<std::size_t>(gram_off[ci]);
            const auto e = static_cast<std::size_t>(gram_off[ci + 1]);
            out->col_grams[ci].assign(grams.begin() + b, grams.begin() + e);
        }
    }
    out->gram_counts.assign(static_cast<std::size_t>(hdr.max_gram_id) + 1u, 0);
    if (hdr.max_gram_id > 0 || !out->gram_counts.empty()) {
        if (!read_exact(
                in,
                out->gram_counts.data(),
                out->gram_counts.size() * sizeof(uint32_t),
                err)) {
            return false;
        }
    }
    return true;
}

bool cybuf_write_native(const std::string& path, const CybufAux& aux, std::string* err) {
    NativeCybufHeader hdr{};
    hdr.magic = NATIVE_CYBF_MAGIC;
    hdr.version = NATIVE_FORMAT_VERSION;
    hdr.n_cols = static_cast<uint32_t>(aux.col_ids.size());
    hdr.max_gram_id = aux.max_gram_id;
    hdr.num_perm = aux.num_perm;
    std::vector<uint32_t> flat_grams;
    std::vector<uint64_t> gram_off(aux.col_ids.size() + 1, 0);
    for (std::size_t i = 0; i < aux.col_grams.size(); ++i) {
        gram_off[i] = flat_grams.size();
        flat_grams.insert(flat_grams.end(), aux.col_grams[i].begin(), aux.col_grams[i].end());
    }
    gram_off[aux.col_ids.size()] = flat_grams.size();
    hdr.n_gram_entries = flat_grams.size();

    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out) {
        if (err) {
            *err = "cannot write cybuf: " + path;
        }
        return false;
    }
    out.write(reinterpret_cast<const char*>(&hdr), sizeof(hdr));
    if (!aux.col_ids.empty()) {
        out.write(reinterpret_cast<const char*>(aux.col_ids.data()),
                  static_cast<std::streamsize>(aux.col_ids.size() * sizeof(uint64_t)));
    }
    if (!gram_off.empty()) {
        out.write(reinterpret_cast<const char*>(gram_off.data()),
                  static_cast<std::streamsize>(gram_off.size() * sizeof(uint64_t)));
    }
    if (!flat_grams.empty()) {
        out.write(reinterpret_cast<const char*>(flat_grams.data()),
                  static_cast<std::streamsize>(flat_grams.size() * sizeof(uint32_t)));
    }
    if (!aux.gram_counts.empty()) {
        out.write(reinterpret_cast<const char*>(aux.gram_counts.data()),
                  static_cast<std::streamsize>(aux.gram_counts.size() * sizeof(uint32_t)));
    }
    return static_cast<bool>(out);
}

bool sig_read_native(const std::string& path, SigBand* out, std::string* err) {
    if (!out) {
        if (err) {
            *err = "null out";
        }
        return false;
    }
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        if (err) {
            *err = "cannot open sig: " + path;
        }
        return false;
    }
    NativeSigBandHeader hdr{};
    if (!read_exact(in, &hdr, sizeof(hdr), err)) {
        return false;
    }
    if (hdr.magic != NATIVE_SIGB_MAGIC || hdr.version != NATIVE_FORMAT_VERSION) {
        if (err) {
            *err = "bad SIGB magic/version";
        }
        return false;
    }
    out->band = hdr.band;
    out->num_perm = hdr.num_perm;
    out->n_cols = hdr.n_cols;
    out->n_samples = 0;
    out->columns.resize(hdr.n_cols);
    for (uint32_t i = 0; i < hdr.n_cols; ++i) {
        ColumnSig col;
        if (!read_exact(in, &col.col_id, sizeof(col.col_id), err)) {
            return false;
        }
        if (!read_exact(in, &col.hit_count, sizeof(col.hit_count), err)) {
            return false;
        }
        if (!read_exact(in, &col.sig_len, sizeof(col.sig_len), err)) {
            return false;
        }
        uint32_t raw_len = 0;
        if (!read_exact(in, &raw_len, sizeof(raw_len), err)) {
            return false;
        }
        col.sig.resize(raw_len);
        if (raw_len > 0 && !read_exact(in, col.sig.data(), raw_len * sizeof(int32_t), err)) {
            return false;
        }
        out->columns[i] = std::move(col);
    }
    return true;
}

bool sig_write_native(const std::string& path, const SigBand& band, std::string* err) {
    uint64_t n_sig_ints = 0;
    for (const ColumnSig& col : band.columns) {
        n_sig_ints += col.sig.size();
    }
    NativeSigBandHeader hdr{};
    hdr.magic = NATIVE_SIGB_MAGIC;
    hdr.version = NATIVE_FORMAT_VERSION;
    hdr.band = band.band;
    hdr.num_perm = band.num_perm;
    hdr.n_cols = static_cast<uint32_t>(band.columns.size());
    hdr.n_sig_ints = n_sig_ints;

    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out) {
        if (err) {
            *err = "cannot write sig: " + path;
        }
        return false;
    }
    out.write(reinterpret_cast<const char*>(&hdr), sizeof(hdr));
    for (const ColumnSig& col : band.columns) {
        out.write(reinterpret_cast<const char*>(&col.col_id), sizeof(col.col_id));
        out.write(reinterpret_cast<const char*>(&col.hit_count), sizeof(col.hit_count));
        out.write(reinterpret_cast<const char*>(&col.sig_len), sizeof(col.sig_len));
        const uint32_t raw_len = static_cast<uint32_t>(col.sig.size());
        out.write(reinterpret_cast<const char*>(&raw_len), sizeof(raw_len));
        if (raw_len > 0) {
            out.write(reinterpret_cast<const char*>(col.sig.data()),
                      static_cast<std::streamsize>(raw_len * sizeof(int32_t)));
        }
    }
    return static_cast<bool>(out);
}

bool sample_order_read_native(const std::string& path, std::vector<int32_t>* out, std::string* err) {
    if (!out) {
        if (err) {
            *err = "null out";
        }
        return false;
    }
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        if (err) {
            *err = "cannot open sample order: " + path;
        }
        return false;
    }
    NativeSampleOrderHeader hdr{};
    if (!read_exact(in, &hdr, sizeof(hdr), err)) {
        return false;
    }
    if (hdr.magic != NATIVE_SORD_MAGIC || hdr.version != NATIVE_FORMAT_VERSION) {
        if (err) {
            *err = "bad SORD magic/version";
        }
        return false;
    }
    out->resize(static_cast<std::size_t>(hdr.n_samples));
    if (hdr.n_samples > 0 &&
        !read_exact(in, out->data(), out->size() * sizeof(int32_t), err)) {
        return false;
    }
    return true;
}

bool sample_order_write_native(const std::string& path, const std::vector<int32_t>& order, std::string* err) {
    NativeSampleOrderHeader hdr{};
    hdr.magic = NATIVE_SORD_MAGIC;
    hdr.version = NATIVE_FORMAT_VERSION;
    hdr.n_samples = order.size();
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out) {
        if (err) {
            *err = "cannot write sample order: " + path;
        }
        return false;
    }
    out.write(reinterpret_cast<const char*>(&hdr), sizeof(hdr));
    if (!order.empty()) {
        out.write(reinterpret_cast<const char*>(order.data()),
                  static_cast<std::streamsize>(order.size() * sizeof(int32_t)));
    }
    return static_cast<bool>(out);
}

bool sig_compare(const SigBand& a, const SigBand& b, std::string* err) {
    if (a.num_perm != b.num_perm || a.columns.size() != b.columns.size()) {
        if (err) {
            *err = "sig header mismatch";
        }
        return false;
    }
    for (std::size_t i = 0; i < a.columns.size(); ++i) {
        const ColumnSig& ca = a.columns[i];
        const ColumnSig& cb = b.columns[i];
        if (ca.col_id != cb.col_id || ca.hit_count != cb.hit_count ||
            ca.sig_len != cb.sig_len || ca.sig != cb.sig) {
            if (err) {
                *err = "sig column mismatch at index " + std::to_string(i);
            }
            return false;
        }
    }
    return true;
}


bool sig_build_replay(
    CybufAux& cy,
    const std::vector<int32_t>& sample_order,
    SigBand* out,
    std::string* err) {
    return sig_build_impl(cy, &sample_order, 0, 0, out, nullptr, nullptr, err);
}

bool sig_build(
    CybufAux& cy,
    int band,
    uint32_t seed,
    SigBand* out,
    std::vector<int32_t>* sample_order_out,
    std::string* err,
    const SigBuildOptions* opts) {
    return sig_build_impl(cy, nullptr, static_cast<int>(seed), static_cast<uint32_t>(band), out,
                          sample_order_out, opts, err);
}

bool cybuf_compare(const CybufAux& a, const CybufAux& b, std::string* err) {
    if (a.num_perm != b.num_perm || a.max_gram_id != b.max_gram_id ||
        a.col_ids != b.col_ids || a.col_grams != b.col_grams || a.gram_counts != b.gram_counts) {
        if (err) {
            *err = "cybuf field mismatch";
        }
        return false;
    }
    return true;
}
