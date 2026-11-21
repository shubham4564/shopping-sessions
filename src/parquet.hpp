#pragma once
#include "input.hpp"
#include <climits>

// Stable C entry points from liblz4.
extern "C" int LZ4_compressBound(int size);
extern "C" int LZ4_compress_fast(const char*, char*, int, int, int);

namespace shop::parquet {
// A flat Parquet v1 writer. Each row group has one PLAIN page per column.
// The only optional field has RLE/bit-packed definition levels of width one.
// Wire definitions: https://github.com/apache/parquet-format/blob/master/src/main/thrift/parquet.thrift
class Bytes {
    std::unique_ptr<char[]> bytes_;
    size_t capacity_ = 0, size_ = 0;

  public:
    size_t size() const { return size_; }
    char* data() const { return bytes_.get(); }
    void clear() { size_ = 0; }
    char* extend(size_t n) {
        if (n > capacity_ - size_) {
            size_t capacity = std::max({size_ + n, capacity_ * 2, size_t(256)});
            std::unique_ptr<char[]> next(new char[capacity]);
            if (size_)
                std::memcpy(next.get(), bytes_.get(), size_);
            bytes_ = std::move(next);
            capacity_ = capacity;
        }
        char* p = bytes_.get() + size_;
        size_ += n;
        return p;
    }
    void append(const void* p, size_t n) {
        if (n)
            std::memcpy(extend(n), p, n);
    }
    template <class T> void number(T n) { std::memcpy(extend(sizeof(T)), &n, sizeof(T)); }
    void string(View value) {
        if (value.size() > INT32_MAX)
            throw std::length_error("Parquet byte array exceeds INT32_MAX");
        char* p = extend(4 + value.size());
        uint32_t n = value.size();
        std::memcpy(p, &n, 4);
        if (n)
            std::memcpy(p + 4, value.data(), n);
    }
    void varint(uint64_t value) {
        while (value >= 128) {
            number(uint8_t(value | 128));
            value >>= 7;
        }
        number(uint8_t(value));
    }
};

// Thrift compact protocol. A scope represents one struct, whose increasing
// field IDs are encoded as deltas from the preceding field ID in that scope.
class Compact {
    Bytes& out_;
    unsigned previous_ = 0;
    void field(unsigned id, unsigned type) {
        out_.number(uint8_t(((id - previous_) << 4) | type));
        previous_ = id;
    }

  public:
    explicit Compact(Bytes& b) : out_(b) {}
    void stop() { out_.number(uint8_t(0)); }
    void integer(unsigned id, int64_t value, bool wide = false) {
        field(id, wide ? 6 : 5);
        out_.varint((uint64_t(value) << 1) ^ uint64_t(-(value < 0)));
    }
    void string(unsigned id, View v) {
        field(id, 8);
        text(v);
    }
    void text(View v) {
        out_.varint(v.size());
        out_.append(v.data(), v.size());
    }
    template <class F> void structure(unsigned id, F fn) {
        field(id, 12);
        Compact nested(out_);
        fn(nested);
        nested.stop();
    }
    template <class F> void list(unsigned id, unsigned type, size_t count, F fn) {
        field(id, 9);
        out_.number(uint8_t((std::min<size_t>(15, count) << 4) | type));
        if (count >= 15)
            out_.varint(count);
        for (size_t i = 0; i < count; ++i)
            fn(i);
    }
    template <class F> void structs(unsigned id, size_t count, F fn) {
        list(id, 12, count, [&](size_t i) {
            Compact item(out_);
            fn(item, i);
            item.stop();
        });
    }
    void encoding(unsigned value) { out_.varint(uint64_t(value) * 2); }
};

enum Physical { i32 = 1, i64 = 2, bytes = 6 };
enum Annotation { none = -1, utf8 = 0, date = 6 };
struct Field {
    View name;
    Physical type;
    Annotation annotation = none;
    bool optional = false;
};

class File {
    struct Column {
        int64_t offset, raw, compressed;
        unsigned codec;
    };
    struct Group {
        uint32_t count;
        int64_t total = 0, encoded = 0;
        std::vector<Column> cols;
    };
    std::ofstream file_;
    std::span<const Field> schema_;
    std::vector<Group> groups_;
    int64_t offset_ = 0, rows_ = 0;
    Bytes header_, compressed_, levels_;
    std::vector<uint8_t> valid_;
    uint32_t n_ = 0;
    void write(const void* p, size_t n) {
        if (n)
            file_.write(static_cast<const char*>(p), n);
        offset_ += n;
    }
    void page(Group& group, unsigned col) {
        const auto& field = schema_[col];
        Bytes* raw = &values[col];
        if (field.optional) {
            levels_.clear();
            uint32_t bytes = (n_ + 7) / 8;
            levels_.number(uint32_t(0));
            levels_.varint(uint64_t(bytes) * 2 + 1);
            levels_.append(valid_.data(), bytes);
            uint32_t length = levels_.size() - 4;
            std::memcpy(levels_.data(), &length, 4);
            levels_.append(raw->data(), raw->size());
            raw = &levels_;
        }
        if (raw->size() > INT32_MAX)
            throw std::length_error("Parquet page exceeds INT32_MAX");
        int size = raw->size(), bound = LZ4_compressBound(size);
        const char* data = raw->data();
        unsigned codec = 0;
        int encoded = size;
        if (size >= 128 && bound > 0) {
            compressed_.clear();
            auto* dest = compressed_.extend(bound);
            int used = LZ4_compress_fast(data, dest, size, bound, 3);
            if (used > 0 && used < size) {
                data = dest;
                encoded = used;
                codec = 7;
            }
        }
        header_.clear();
        Compact h(header_);
        h.integer(1, 0);
        h.integer(2, size);
        h.integer(3, encoded);
        h.structure(5, [&](Compact& d) {
            d.integer(1, n_);
            d.integer(2, 0);
            d.integer(3, 3);
            d.integer(4, 3);
        });
        h.stop();
        int64_t payload = header_.size();
        group.cols.push_back({offset_, size + payload, encoded + payload, codec});
        group.total += size + payload;
        group.encoded += encoded + payload;
        write(header_.data(), header_.size());
        write(data, encoded);
        values[col].clear();
    }
    void flush() {
        if (!n_)
            return;
        auto& group = groups_.emplace_back();
        group.count = n_;
        group.cols.reserve(schema_.size());
        for (unsigned i = 0; i < schema_.size(); ++i)
            page(group, i);
        std::fill_n(valid_.begin(), (n_ + 7) / 8, 0);
        rows_ += n_;
        n_ = 0;
    }

  public:
    static constexpr unsigned row_group = 32768;
    std::vector<Bytes> values;
    File(const fs::path& path, std::span<const Field> schema)
        : file_(path, std::ios::binary), schema_(schema), valid_((row_group + 7) / 8),
          values(schema.size()) {
        file_.exceptions(std::ios::failbit | std::ios::badbit);
        write("PAR1", 4);
    }
    uint32_t available() const { return row_group - n_; }
    void defined(uint32_t offset) {
        uint32_t row = n_ + offset;
        valid_[row >> 3] |= uint8_t(1u << (row & 7));
    }
    void finish_rows(uint32_t count) {
        n_ += count;
        if (n_ == row_group)
            flush();
    }
    void finish_row() { finish_rows(1); }
    void close() {
        flush();
        Bytes footer;
        Compact f(footer);
        f.integer(1, 1);
        f.structs(2, schema_.size() + 1, [&](Compact& e, size_t i) {
            if (!i) {
                e.string(4, "schema");
                e.integer(5, schema_.size());
                return;
            }
            const auto& s = schema_[i - 1];
            e.integer(1, s.type);
            e.integer(3, s.optional);
            e.string(4, s.name);
            if (s.annotation != none) {
                e.integer(6, s.annotation);
                e.structure(
                    10, [&](Compact& t) { t.structure(s.annotation == utf8 ? 1 : 6, [](Compact&) {}); });
            }
        });
        f.integer(3, rows_, true);
        f.structs(4, groups_.size(), [&](Compact& g, size_t j) {
            const auto& group = groups_[j];
            g.structs(1, schema_.size(), [&](Compact& chunk, size_t i) {
                const auto& field = schema_[i];
                const auto& c = group.cols[i];
                chunk.integer(2, 0, true);
                chunk.structure(3, [&](Compact& m) {
                    m.integer(1, field.type);
                    m.list(2, 5, field.optional ? 2 : 1, [&](size_t k) { m.encoding(k ? 3 : 0); });
                    m.list(3, 8, 1, [&](size_t) { m.text(field.name); });
                    m.integer(4, c.codec);
                    m.integer(5, group.count, true);
                    m.integer(6, c.raw, true);
                    m.integer(7, c.compressed, true);
                    m.integer(9, c.offset, true);
                });
            });
            g.integer(2, group.total, true);
            g.integer(3, group.count, true);
            g.integer(6, group.encoded, true);
        });
        f.string(6, "shopping-sessions");
        f.stop();
        if (footer.size() > UINT32_MAX)
            throw std::length_error("Parquet footer exceeds UINT32_MAX");
        uint32_t size = footer.size();
        write(footer.data(), size);
        write(&size, 4);
        write("PAR1", 4);
        file_.close();
    }
};
} // namespace shop::parquet
