#pragma once
#include "input.hpp"

namespace shop {
#if defined(__AVX2__) && defined(__BMI2__)
// Check quote pairing and delimiter counts in whole blocks. Return the
// actual LF offsets so that fast field readers can work on the original mapping.
template <size_t N> int verify_lines(View input, uint16_t* lines) {
    static constexpr auto expected = newline_pattern<N>();
    const char* p = input.data();
    unsigned sep_count = 0, nlines = 0, previous_sep = 1, previous_quote = 0, open_field = p[0] == '"';
    for (unsigned i = 0; i < input.size(); i += 64) {
        const unsigned n = std::min<size_t>(64, input.size() - i);
        const uint64_t valid = low_bits(n);
        // The caller guarantees 128 readable padding bytes after the block.
#if defined(__AVX512BW__)
        __m512i x = _mm512_loadu_si512(p + i);
        // This fast block also proves every payload byte is nonzero ASCII.
        // The scalar grammar accepts arbitrary UTF-8 and literal NUL.
        if (_mm512_cmp_epi8_mask(x, _mm512_setzero_si512(), _MM_CMPINT_LE) & valid)
            return -1;
        uint64_t lf = _mm512_cmpeq_epi8_mask(x, _mm512_set1_epi8('\n')) & valid;
        uint64_t q = _mm512_cmpeq_epi8_mask(x, _mm512_set1_epi8('"')) & valid;
        uint64_t sep = lf | (_mm512_cmpeq_epi8_mask(x, _mm512_set1_epi8(',')) & valid);
#else
        __m256i a = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(p + i));
        __m256i b = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(p + i + 32));
        auto zero = _mm256_setzero_si256();
        uint64_t clean = uint32_t(_mm256_movemask_epi8(_mm256_cmpgt_epi8(a, zero))) |
                         (uint64_t(uint32_t(_mm256_movemask_epi8(_mm256_cmpgt_epi8(b, zero)))) << 32);
        if ((~clean) & valid)
            return -1;
        auto equal = [&](char c) {
            auto match = _mm256_set1_epi8(c);
            uint64_t lo = uint32_t(_mm256_movemask_epi8(_mm256_cmpeq_epi8(a, match)));
            uint64_t hi = uint32_t(_mm256_movemask_epi8(_mm256_cmpeq_epi8(b, match)));
            return (lo | (hi << 32)) & valid;
        };
        uint64_t lf = equal('\n'), q = equal('"'), sep = lf | equal(',');
#endif
        unsigned ns = std::popcount(sep);
        auto field_bits = low_bits(ns);
        if (_pext_u64(lf, sep) != (expected[sep_count % N] & field_bits))
            return -1;
        char next = i + 64 < input.size() ? p[i + 64] : '\0';
        uint64_t adjacent_left = (sep << 1) | previous_sep;
        uint64_t adjacent_right = (sep >> 1) | (uint64_t(next == ',' || next == '\n') << 63);
        uint64_t endq = (q << 1) | previous_quote;
        uint64_t startq = _pext_u64((q >> 1) | (uint64_t(next == '"') << 63), sep);
        if ((q & ~(adjacent_left ^ adjacent_right)) ||
            ((_pext_u64(endq, sep) ^ ((startq << 1) | open_field)) & field_bits))
            return -1;
        if (ns)
            open_field = (startq >> (ns - 1)) & 1;
        for (uint64_t bits = lf; bits; bits &= bits - 1)
            lines[nlines++] = i + std::countr_zero(bits);
        sep_count += ns;
        previous_sep = sep >> 63;
        previous_quote = q >> 63;
    }
    return sep_count == N * nlines ? int(nlines) : -1;
}
template <size_t N, class Fast, class Slow> void plain_rows(View chunk, Fast&& fast, Slow&& slow) {
    alignas(64) uint16_t lines[4352];
    while (!chunk.empty()) {
        size_t size = chunk.size();
        if (size > 4096) {
            auto lf = chunk.find('\n', 4095);
            size = lf == View::npos ? size : lf + 1;
        }
        View part = chunk.substr(0, size);
        int nr = (size <= 4352 && chunk.size() >= size + 128 && part.back() == '\n')
                     ? verify_lines<N>(part, lines)
                     : -1;
        if (nr < 0) {
            // Isolate malformed lines to ~1KiB sub-blocks so clean lines stay on the SIMD path.
            if (part.size() > 1024 && chunk.size() >= size + 128 && part.back() == '\n') {
                View rem = part;
                while (!rem.empty()) {
                    size_t sub = rem.size();
                    if (sub > 1024) {
                        auto lf = rem.find('\n', 1023);
                        sub = lf == View::npos ? sub : lf + 1;
                    }
                    View sp = rem.substr(0, sub);
                    int snr = (sub <= 4352 && sp.back() == '\n') ? verify_lines<N>(sp, lines) : -1;
                    if (snr < 0)
                        scalar_rows<N>(sp, slow);
                    else {
                        const char* text = sp.data();
                        unsigned begin = 0;
                        for (int n = 0; n < snr; ++n) {
                            unsigned end = lines[n];
                            if (!fast(text + begin, text + end))
                                scalar_rows<N>(View(text + begin, end - begin), slow);
                            begin = end + 1;
                        }
                    }
                    rem.remove_prefix(sub);
                }
            } else {
                scalar_rows<N>(part, slow);
            }
        } else {
            const char* text = part.data();
            unsigned begin = 0;
            for (int n = 0; n < nr; ++n) {
                unsigned end = lines[n];
                if (!fast(text + begin, text + end))
                    scalar_rows<N>(View(text + begin, end - begin), slow);
                begin = end + 1;
            }
        }
        chunk.remove_prefix(size);
    }
}
#endif
} // namespace shop
