#pragma once
#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <limits>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <vector>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

namespace shop {
namespace fs = std::filesystem;
using View = std::string_view;
using Key = uint64_t;
constexpr int64_t day_ms = 86400000, week_ms = 7 * day_ms, gap_ms = 1800000;
constexpr int64_t bad_time = std::numeric_limits<int64_t>::min();

inline uint64_t mix(uint64_t x) {
    x ^= x >> 30;
    x *= 0xbf58476d1ce4e5b9ULL;
    x ^= x >> 27;
    x *= 0x94d049bb133111ebULL;
    return x ^ (x >> 31);
}

class Mapped {
    const char* bytes_ = nullptr;
    size_t size_ = 0;
public:
    explicit Mapped(const fs::path& path) {
        int fd = open(path.c_str(), O_RDONLY | O_CLOEXEC);
        if (fd < 0) throw std::runtime_error("cannot open " + path.string());
        struct stat info{};
        if (fstat(fd, &info) != 0) {
            close(fd);
            throw std::runtime_error("cannot stat " + path.string());
        }
        size_ = info.st_size;
        if (size_) {
            void* p = mmap(nullptr, size_, PROT_READ, MAP_PRIVATE, fd, 0);
            if (p == MAP_FAILED) {
                close(fd);
                throw std::runtime_error("cannot map " + path.string());
            }
            bytes_ = static_cast<const char*>(p);
        }
        close(fd);
    }
    Mapped(const Mapped&) = delete;
    ~Mapped() { if (bytes_) munmap(const_cast<char*>(bytes_), size_); }
    View view() const { return size_ ? View(bytes_, size_) : View{}; }
};

// Only the worker that owns a line's first byte parses it. Finding line
// boundaries here keeps the coordinator from faulting in every input file.
struct Chunk {
    const char* begin;
    const char* stop;
    const char* eof;
    bool first;
    operator View() const {
        const char* p = begin;
        if (first || p[-1] != '\n') {
            const auto* lf = static_cast<const char*>(std::memchr(p, '\n', stop - p));
            if (!lf) return {};
            p = lf + 1;
        }
        if (p == stop) return {};
        const char* end = stop;
        if (end < eof && end[-1] != '\n') {
            const auto* lf = static_cast<const char*>(std::memchr(end, '\n', eof - end));
            end = lf ? lf + 1 : eof;
        }
        return View(p, end - p);
    }
};

struct Input {
    std::vector<std::unique_ptr<Mapped>> files;
    std::vector<Chunk> chunks;
    size_t size = 0;

    void add(const fs::path& path) {
        files.emplace_back(std::make_unique<Mapped>(path));
        View rest = files.back()->view();
        size += rest.size();
        constexpr size_t block = 1 << 20;
        for (size_t at = 0; at < rest.size(); at += block) {
            auto p = rest.data();
            chunks.push_back({p + at, p + std::min(at + block, rest.size()),
                              p + rest.size(), at == 0});
        }
    }

    explicit Input(const fs::path& path) {
        if (fs::is_directory(path)) {
            std::vector<fs::path> parts;
            for (const auto& part : fs::directory_iterator(path))
                if (part.is_regular_file() && part.path().extension() == ".csv")
                    parts.push_back(part.path());
            std::sort(parts.begin(), parts.end());
            for (const auto& part : parts) add(part);
        } else {
            add(path);
        }
    }
};

// LF is the only record delimiter. Commas always delimit fields, even on
// malformed lines. RFC CSV readers can silently accept or merge such lines.
template<size_t N, class F>
void rows(View chunk, F&& consume) {
    if (chunk.empty()) return;
    const char* p = chunk.data();
    const char* end = p + chunk.size();
    while (p < end) {
        const auto* lf = static_cast<const char*>(std::memchr(p, '\n', end - p));
        const char* stop = lf ? lf : end;
        const char* field = p;
        p = lf ? lf + 1 : end;
        std::array<View, N> values;
        bool valid = true;
        for (size_t i = 0; i < N; ++i) {
            const auto* comma = static_cast<const char*>(std::memchr(field, ',', stop - field));
            if ((i + 1 < N) != (comma != nullptr)) { valid = false; break; }
            View s(field, (comma ? comma : stop) - field);
            if (!s.empty() && s.front() == '"') {
                if (s.size() < 2 || s.back() != '"') { valid = false; break; }
                s.remove_prefix(1);
                s.remove_suffix(1);
            }
            if (s.find('"') != View::npos) { valid = false; break; }
            values[i] = s;
            if (comma) field = comma + 1;
        }
        if (valid) consume(values);
    }
}

template<class F>
void jobs(size_t count, unsigned threads, F&& work) {
    if (!count) return;
    threads = static_cast<unsigned>(std::min<size_t>(count, threads));
    std::atomic<size_t> next{0};
    std::exception_ptr error;
    std::mutex lock;
    auto worker = [&](unsigned lane) {
        try {
            for (;;) {
                size_t i = next.fetch_add(1, std::memory_order_relaxed);
                if (i >= count) return;
                work(lane, i);
            }
        } catch (...) {
            std::lock_guard guard(lock);
            if (!error) error = std::current_exception();
            next.store(count, std::memory_order_relaxed);
        }
    };
    std::vector<std::jthread> pool;
    for (unsigned t = 1; t < threads; ++t) pool.emplace_back(worker, t);
    worker(0);
    pool.clear();
    if (error) std::rethrow_exception(error);
}

// Short nonzero ASCII strings fit losslessly in nine 7-bit slots. Unsigned
// comparison gives their byte order. Every other ID is interned by exact bytes.
// All source mappings outlive the dictionary and its readers.
class Keys {
    static constexpr Key external = Key{1} << 63;
    struct Bucket {
        std::mutex mutex;
        std::unordered_map<View, Key> intern;
        std::vector<View> reverse;
    };
    std::array<Bucket, 64> buckets_;

    Key intern(View s) {
        size_t bucket = std::hash<View>{}(s) % buckets_.size();
        auto& b = buckets_[bucket];
        std::lock_guard lock(b.mutex);
        auto [it, is_new] = b.intern.try_emplace(s, external | (Key(b.reverse.size()) << 6) | bucket);
        if (is_new) b.reverse.push_back(s);
        return it->second;
    }
public:
    Key encode(View s) {
        if (s.size() <= 9) {
            Key k = 0;
            for (unsigned char c : s) {
                if (c == 0 || c >= 128) return intern(s);
                k = (k << 7) | c;
            }
            return k << (7 * (9 - s.size()));
        }
        return intern(s);
    }
    // Called between input phases, or after parsing has finished.
    View decode(Key k, char (&buf)[9]) const {
        if (k & external) return buckets_[k & 63].reverse[(k ^ external) >> 6];
        size_t n = 0;
        for (int shift = 56; shift >= 0; shift -= 7) {
            char c = static_cast<char>((k >> shift) & 127);
            if (!c) break;
            buf[n++] = c;
        }
        return View(buf, n);
    }
    bool less(Key a, Key b) const {
        if (((a | b) & external) == 0) return a < b;
        char x[9], y[9];
        return decode(a, x) < decode(b, y);
    }
};

// A single negative result is sufficient for malformed or out-of-range
// nonnegative decimals. Arbitrarily many leading zeros remain significant
// only for syntax, never for overflow.
inline int64_t decimal(View s, int64_t limit) {
    if (s.empty()) return -1;
    int64_t n = 0;
    for (unsigned char c : s) {
        unsigned d = unsigned(c) - '0';
        if (d > 9 || n > limit / 10 || (n == limit / 10 && d > unsigned(limit % 10))) return -1;
        n = n * 10 + d;
    }
    return n;
}

inline int pair_at(const char* p) {
    unsigned a = static_cast<unsigned char>(p[0]) - unsigned('0');
    unsigned b = static_cast<unsigned char>(p[1]) - unsigned('0');
    return a <= 9 && b <= 9 ? static_cast<int>(a * 10 + b) : -1;
}
inline int civil_days(int year, int month, int day) {
    year -= month <= 2;
    int era = year / 400, y = year % 400;
    int adjusted = month + (month > 2 ? -3 : 9);
    return era * 146097 + 365 * y + y / 4 - y / 100 + (153 * adjusted + 2) / 5 + day - 1 - 719468;
}
inline int64_t timestamp(View s) {
    if ((s.size() != 20 && s.size() != 25) || s[4] != '-') {
        int64_t n = decimal(s, std::numeric_limits<int64_t>::max());
        return n < 0 ? bad_time : n;
    }
    const char* p = s.data();
    if (p[7]!='-' || p[10]!='T' || p[13]!=':' || p[16]!=':') return bad_time;
    int hi = pair_at(p), lo = pair_at(p+2), month = pair_at(p+5), day = pair_at(p+8);
    int hour = pair_at(p+11), minute = pair_at(p+14), second = pair_at(p+17);
    if (hi < 0 || lo < 0 || month < 1 || month > 12 || day < 1 ||
        unsigned(hour) > 23 || unsigned(minute) > 59 || unsigned(second) > 59) return bad_time;
    int year = hi * 100 + lo;
    static constexpr int lengths[] = {31,28,31,30,31,30,31,31,30,31,30,31};
    bool leap_day = month == 2 && year % 4 == 0 && (year % 100 != 0 || year % 400 == 0);
    if (!year || day > lengths[month-1] + leap_day) return bad_time;
    int offset = 0;
    if (s.size() == 20) {
        if (p[19] != 'Z') return bad_time;
    } else {
        if ((p[19] != '+' && p[19] != '-') || p[22] != ':') return bad_time;
        int h = pair_at(p+20), m = pair_at(p+23);
        if (unsigned(h) > 23 || unsigned(m) > 59) return bad_time;
        offset = (h * 60 + m) * (p[19] == '-' ? -1 : 1);
    }
    return int64_t(civil_days(year, month, day)) * day_ms +
           ((hour * 60 + minute - offset) * 60 + second) * int64_t(1000);
}
inline int32_t utc_day(int64_t ts) {
    return static_cast<int32_t>(ts / day_ms - (ts % day_ms < 0));
}

struct Window { int64_t start, end; };
inline Window read_window(const fs::path& path) {
    Mapped file(path);
    View json = file.view();
    auto value = [&](View key) {
        auto pos = json.find(key);
        if (pos != View::npos) pos = json.find(':', pos + key.size());
        if (pos != View::npos) pos = json.find('"', pos + 1);
        if (pos == View::npos) throw std::runtime_error("missing manifest date");
        size_t end = json.find('"', pos + 1);
        if (end == View::npos) throw std::runtime_error("unterminated manifest date");
        return timestamp(json.substr(pos + 1, end - pos - 1));
    };
    Window w{value("\"window_start\""), value("\"window_end\"")};
    if (w.start == bad_time || w.end == bad_time || w.start >= w.end)
        throw std::runtime_error("invalid manifest window");
    return w;
}
}
