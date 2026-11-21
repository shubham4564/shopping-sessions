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
#include <immintrin.h>
#include <future>
#include <deque>
#include <span>
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
void scalar_rows(View chunk, F&& consume) {
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

#if defined(__AVX2__) && defined(__BMI2__)
template<size_t N> constexpr auto newline_pattern() {
    std::array<uint64_t,N> table{};
    for(unsigned start=0;start<N;++start)
        for(unsigned i=0;i<64;++i)
            if ((start+i)%N==N-1) table[start]|=uint64_t{1}<<i;
    return table;
}

inline uint64_t low_bits(unsigned n) {
    return _bzhi_u64(~uint64_t{0},n);
}

#endif

#if defined(__AVX512BW__) && defined(__AVX512VBMI2__)
// A descriptor stores the byte offset of a delimiter and one quote bit.
// Validate entire blocks before consuming rows. A structurally irregular
// block is replayed with the scalar grammar to isolate individual bad rows.
template<size_t N, class F>
bool indexed_rows(View input, F& consume, uint16_t* fields) {
    static constexpr auto expected=newline_pattern<N>();
    alignas(64) static constexpr auto seq=[]{
        std::array<uint16_t,32> a{};
        for(unsigned i=0;i<a.size();++i) a[i]=i;
        return a;
    }();
    const char* p=input.data();
    const size_t size=input.size();
    unsigned count=0, previous_sep=1, previous_quote=0, open_field=p[0]=='"';
    __m512i index=_mm512_load_si512(seq.data());
    for(unsigned i=0;i<size;i+=64) {
        const unsigned n=std::min<size_t>(64,size-i);
        __m512i x = n==64 ? _mm512_loadu_si512(p+i) :
            _mm512_maskz_loadu_epi8(low_bits(n),p+i);
        uint64_t lf=_mm512_cmpeq_epi8_mask(x,_mm512_set1_epi8('\n'));
        uint64_t sep=lf|_mm512_cmpeq_epi8_mask(x,_mm512_set1_epi8(','));
        uint64_t q=_mm512_cmpeq_epi8_mask(x,_mm512_set1_epi8('"'));
        unsigned ns=std::popcount(sep);
        uint64_t valid=low_bits(ns);
        if (_pext_u64(lf,sep)!=(expected[count%N]&valid)) return false;
        char next=i+64<size ? p[i+64] : '\0';
        uint64_t before=(sep<<1)|previous_sep;
        uint64_t after=(sep>>1)|(uint64_t(next==','||next=='\n')<<63);
        if(q & ~(before^after)) return false;
        uint64_t quoted_end=(q<<1)|previous_quote;
        uint64_t starts=_pext_u64((q>>1)|(uint64_t(next=='"')<<63),sep);
        if ((_pext_u64(quoted_end,sep)^((starts<<1)|open_field))&valid) return false;
        if(ns) open_field=(starts>>(ns-1))&1;
        const __m512i flag=_mm512_set1_epi16(short(0x8000));
        __m512i lower=_mm512_mask_add_epi16(index,uint32_t(quoted_end),index,flag);
        _mm512_mask_compressstoreu_epi16(fields+count,uint32_t(sep),lower);
        __m512i high=_mm512_add_epi16(index,_mm512_set1_epi16(32));
        high=_mm512_mask_add_epi16(high,uint32_t(quoted_end>>32),high,flag);
        _mm512_mask_compressstoreu_epi16(fields+count+std::popcount(uint32_t(sep)),uint32_t(sep>>32),high);
        index=_mm512_add_epi16(index,_mm512_set1_epi16(64));
        count+=ns;
        previous_sep=sep>>63; previous_quote=q>>63;
    }
    if(count%N) return false;
    unsigned begin=0;
    for(unsigned row=0;row<count;row+=N) {
        std::array<View,N> out;
        for(unsigned k=0;k<N;++k) {
            unsigned d=fields[row+k], quoted=d>>15, end=d&32767;
            out[k]=View(p+begin+quoted,end-begin-2*quoted);
            begin=end+1;
        }
        consume(out);
    }
    return true;
}
template<size_t N, class F>
void vector_rows(View chunk,F&& consume) {
    alignas(64) uint16_t fields[4352];
    while(!chunk.empty()) {
        size_t size=chunk.size();
        if(size>4096) {
            auto lf=chunk.find('\n',4095);
            size=lf==View::npos ? size : lf+1;
        }
        View part=chunk.substr(0,size);
        if(size>4352 || part.back()!='\n' || !indexed_rows<N>(part,consume,fields))
            scalar_rows<N>(part,consume);
        chunk.remove_prefix(size);
    }
}
#endif

template<size_t N, class F>
void rows(View chunk, F&& consume) {
    if (chunk.empty()) return;
#if defined(__AVX512BW__) && defined(__AVX512VBMI2__)
    vector_rows<N>(chunk,consume);
#else
    scalar_rows<N>(chunk,consume);
#endif
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
// Interned strings own their storage independently of the source mappings.
class Keys {
    static constexpr Key external = Key{1} << 63;
    struct Bucket {
        mutable std::mutex mutex;
        std::unordered_map<View, Key> intern;
        std::vector<View> reverse;
        std::deque<std::string> owned;
    };
    std::array<Bucket, 64> buckets_;

    Key intern(View s) {
        size_t bucket = std::hash<View>{}(s) % buckets_.size();
        auto& b = buckets_[bucket];
        std::lock_guard lock(b.mutex);
        auto found=b.intern.find(s);
        if(found!=b.intern.end()) return found->second;
        Key id=external | (Key(b.reverse.size()) << 6) | bucket;
        b.owned.emplace_back(s);
        View stable=b.owned.back();
        b.reverse.push_back(stable);
        b.intern.emplace(stable,id);
        return id;
    }
public:
    Key clean(const char* p,size_t n) {
#if defined(__BMI2__)
        if(n<=9) {
            uint64_t w;
            std::memcpy(&w,p,8);
            Key k=(_pext_u64(__builtin_bswap64(w),0x7f7f7f7f7f7f7f7fULL)<<7)|
                (static_cast<unsigned char>(p[8])&127u);
            return k & (~Key{0}<<(7*(9-n)));
        }
#endif
        return encode(View(p,n));
    }
    Key clean_unordered(const char* p, size_t n) {
#if defined(__BMI2__)
        if (n <= 9) {
            uint64_t w;
            std::memcpy(&w, p, 8);
            if (n == 9)
                return _pext_u64(w, 0x7f7f7f7f7f7f7f7fULL) | (uint64_t(static_cast<unsigned char>(p[8])) << 56);
            return _pext_u64(_bzhi_u64(w, unsigned(n * 8)), 0x7f7f7f7f7f7f7f7fULL);
        }
#endif
        return encode_unordered(View(p, n));
    }
    Key encode_unordered(View s) {
#if defined(__BMI2__)
        if (s.size() == 9 || s.size() == 8) {
            uint64_t word;
            std::memcpy(&word, s.data(), 8);
            constexpr uint64_t high = 0x8080808080808080ULL;
            unsigned char ninth = s.size() == 9 ? s[8] : 1;
            if (!((((word - 0x0101010101010101ULL) & ~word) | word) & high) &&
                ninth && ninth < 128)
                return _pext_u64(word, ~high) | (s.size() == 9 ? (uint64_t(ninth) << 56) : 0);
        }
#endif
        if (s.size() <= 9) {
            Key k = 0;
            for (size_t i = 0; i < s.size(); ++i) {
                unsigned char c = static_cast<unsigned char>(s[i]);
                if (c == 0 || c >= 128) return intern(s);
                k |= Key(c) << (7 * i);
            }
            return k;
        }
        return intern(s);
    }
    Key encode(View s) {
#if defined(__BMI2__)
        if (s.size() == 9 || s.size() == 8) {
            uint64_t word;
            std::memcpy(&word, s.data(), 8);
            constexpr uint64_t high = 0x8080808080808080ULL;
            unsigned char ninth = s.size() == 9 ? s[8] : 1;
            if (!((((word - 0x0101010101010101ULL) & ~word) | word) & high) &&
                ninth && ninth < 128)
                return (_pext_u64(__builtin_bswap64(word), ~high) << 7) |
                       (s.size() == 9 ? ninth : 0);
        }
#endif
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
        if (k & external) {
            const auto& bucket = buckets_[k & 63];
            std::lock_guard lock(bucket.mutex);
            return bucket.reverse[(k ^ external) >> 6];
        }
#if defined(__BMI2__)
        uint64_t bytes = __builtin_bswap64(_pdep_u64(k >> 7, 0x7f7f7f7f7f7f7f7fULL));
        std::memcpy(buf, &bytes, 8);
        buf[8] = k & 127;
        return View(buf, k ? 9 - std::countr_zero(k) / 7 : 0);
#endif
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
