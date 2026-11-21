#pragma once
#include "input.hpp"
#include <ctime>

namespace shop {
inline uint16_t read16(const char* p) {
    uint16_t v;
    std::memcpy(&v, p, 2);
    return v;
}
inline uint64_t read64(const char* p) {
    uint64_t v;
    std::memcpy(&v, p, 8);
    return v;
}

// Populated from the manifest. Most input dates can be checked with a prefix
// comparison and four two-byte lookups. Dates outside the cache use the full
// civil-date parser, including unusual ingest times on duplicate copies.
class Dates {
    struct Month {
        uint64_t prefix = 0;
        int64_t base = 0, relative_seconds = 0;
        uint8_t days = 0;
    };
    std::array<Month, 16> months_{};
    int64_t window_seconds_;
    alignas(64) std::array<uint8_t, 65536> digits_{};

    static bool digits8(uint64_t w) {
        return (((w + 0x4646464646464646ULL) | (w - 0x3030303030303030ULL)) & 0x8080808080808080ULL) == 0;
    }
    static uint32_t number8(uint64_t w) {
        w = (w & 0x0f0f0f0f0f0f0f0fULL) * 2561 >> 8;
        w = (w & 0x00ff00ff00ff00ffULL) * 6553601 >> 16;
        return uint32_t(((w & 0x0000ffff0000ffffULL) * 42949672960001ULL) >> 32);
    }
#if defined(__SSSE3__) && defined(__SSE4_1__)
    static __m128i clock_digits(const char* p) {
        auto s = _mm_sub_epi8(_mm_loadu_si128(reinterpret_cast<const __m128i*>(p + 4)), _mm_set1_epi8('0'));
        return _mm_shuffle_epi8(s,
                                _mm_setr_epi8(4, 5, 7, 8, 10, 11, 13, 14, -1, -1, -1, -1, -1, -1, -1, -1));
    }
    static int clock_seconds(__m128i pairs) {
        auto totals = _mm_madd_epi16(pairs, _mm_setr_epi16(24, 1, 60, 1, 0, 0, 0, 0));
        return _mm_cvtsi128_si32(totals) * 3600 + _mm_extract_epi32(totals, 1);
    }
#endif
  public:
    // The grammar is identical to parse(). All padded calls originate in a
    // structurally checked block with at least 128 readable bytes after it.
    int64_t padded(const char* p, unsigned n) const {
        if (n == 13) {
            uint64_t first = read64(p), last = read64(p + 5);
            if (digits8(first) && digits8(last))
                return int64_t(number8(first)) * 100000 + number8(last) % 100000;
            return bad_time;
        }
        if (n == 20 || n == 25) {
            const Month& m = months_[p[6] & 15];
            if (read64(p) != m.prefix)
                return timestamp(View(p, n));
            unsigned day = digits_[read16(p + 8)], h = digits_[read16(p + 11)],
                     min = digits_[read16(p + 14)], sec = digits_[read16(p + 17)];
            if (p[10] != 'T' || p[13] != ':' || p[16] != ':' || (day - 1u) >= m.days ||
                h >= 24u || min >= 60u || sec >= 60u)
                return bad_time;
            int offset = 0;
            if (n == 20) {
                if (p[19] != 'Z')
                    return bad_time;
            } else {
                unsigned oh = digits_[read16(p + 20)], om = digits_[read16(p + 23)];
                if ((p[19] != '+' && p[19] != '-') || p[22] != ':' || oh >= 24u || om >= 60u)
                    return bad_time;
                offset = int(oh * 60 + om) * (p[19] == '-' ? -1 : 1);
            }
            return m.base + (int64_t(day) * 86400 + int64_t((int(h) * 60 + int(min) - offset) * 60 + int(sec))) * 1000;
        }
        return parse(View(p, n));
    }
#if defined(__AVX2__)
    // Decode the event and its guaranteed-valid UTC ingestion timestamp in
    // separate 128-bit lanes. The event still undergoes every grammar check.
    void paired_utc(const char* event, const char* ingest, int64_t& ts, int64_t& ing) const {
        const Month& m = months_[event[6] & 15];
        if (read64(event) != m.prefix || read64(ingest) != m.prefix) {
            ts = padded(event, 20);
            ing = relative_ingest(ingest);
            return;
        }
        unsigned d = digits_[read16(event + 8)], h = digits_[read16(event + 11)],
                 mi = digits_[read16(event + 14)], s = digits_[read16(event + 17)];
        unsigned id = digits_[read16(ingest + 8)], ih = digits_[read16(ingest + 11)],
                 imi = digits_[read16(ingest + 14)], is = digits_[read16(ingest + 17)];
        ing = m.relative_seconds + int64_t(id) * 86400 + int64_t((ih * 60 + imi) * 60 + is);
        if ((d - 1u) >= m.days || h >= 24u || mi >= 60u || s >= 60u ||
            event[10] != 'T' || event[13] != ':' || event[16] != ':' || event[19] != 'Z') {
            ts = bad_time;
            return;
        }
        ts = m.base + (int64_t(d) * 86400 + int64_t((h * 60 + mi) * 60 + s)) * 1000;
    }
#endif
    explicit Dates(Window w) : window_seconds_(w.start / 1000) {
        digits_.fill(255);
        for (int i = 0; i < 100; ++i)
            digits_[('0' + i / 10) | (('0' + i % 10) << 8)] = i;
        // Visit a small range around each end. Prefixes are always verified.
        for (int64_t base : {w.end, w.start})
            for (int offset = -35; offset <= 35; offset += 7) {
                std::time_t seconds = base / 1000 + int64_t(offset) * 86400;
                std::tm t{};
                if (!gmtime_r(&seconds, &t) || t.tm_year < -1899 || t.tm_year > 8099)
                    continue;
                int year = t.tm_year + 1900, month = t.tm_mon + 1;
                char prefix[32];
                std::snprintf(prefix, sizeof(prefix), "%04d-%02d-", year, month);
                int next = civil_days(year + (month == 12), month == 12 ? 1 : month + 1, 1);
                int first = civil_days(year, month, 1);
                months_[prefix[6] & 15] = {read64(prefix), int64_t(first - 1) * day_ms,
                                           int64_t(first - 1) * 86400 - window_seconds_,
                                           uint8_t(next - first)};
            }
    }
    int64_t iso(View s) const {
        const char* p = s.data();
        const Month& m = months_[p[6] & 15];
        if (read64(p) != m.prefix)
            return timestamp(s);
        int day = digits_[read16(p + 8)], h = digits_[read16(p + 11)], min = digits_[read16(p + 14)],
            sec = digits_[read16(p + 17)];
        if (p[10] != 'T' || p[13] != ':' || p[16] != ':' || unsigned(day - 1) >= m.days || h >= 24 ||
            min >= 60 || sec >= 60)
            return bad_time;
        int offset = 0;
        if (s.size() == 20) {
            if (p[19] != 'Z')
                return bad_time;
        } else {
            int oh = digits_[read16(p + 20)], om = digits_[read16(p + 23)];
            if ((p[19] != '+' && p[19] != '-') || p[22] != ':' || oh >= 24 || om >= 60)
                return bad_time;
            offset = (oh * 60 + om) * (p[19] == '-' ? -1 : 1);
        }
        return m.base + int64_t(day) * day_ms + int64_t((h * 60 + min - offset) * 60 + sec) * 1000;
    }
    int64_t relative_ingest(const char* p) const {
        const Month& m = months_[p[6] & 15];
        if (read64(p) != m.prefix) {
            int64_t ms = timestamp(View(p, 20));
            return ms == bad_time ? bad_time : ms / 1000 - window_seconds_;
        }
        return m.relative_seconds + int64_t(digits_[read16(p + 8)]) * 86400 +
               (digits_[read16(p + 11)] * 60 + digits_[read16(p + 14)]) * 60 + digits_[read16(p + 17)];
    }
    int64_t ingest(const char* p) const {
        int64_t delta = relative_ingest(p);
        return delta == bad_time ? bad_time : (window_seconds_ + delta) * 1000;
    }
    int64_t parse(View s) const {
        if (s.size() == 13) {
            uint64_t first = read64(s.data()), last = read64(s.data() + 5);
            if (digits8(first) && digits8(last))
                return int64_t(number8(first)) * 100000 + number8(last) % 100000;
            return bad_time;
        }
        if ((s.size() == 20 || s.size() == 25) && s[4] == '-')
            return iso(s);
        return timestamp(s);
    }
};
} // namespace shop
