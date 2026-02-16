#ifndef MASKED_BMH_H
#define MASKED_BMH_H

#include <cstddef>
#include <cstdint>
#include <array>

// Global masked BMH search helper (header-only)
static inline size_t masked_bmh_search(const uint8_t *haystack, size_t hay_len,
                                       const uint8_t *needle, const char *mask, size_t nlen,
                                       size_t start_offset = 0, size_t alignment = 1)
{
    if (nlen == 0 || hay_len < nlen) return SIZE_MAX;
    bool has_wild = false;
    for (size_t i = 0; i < nlen; ++i) if (mask[i] == '?') { has_wild = true; break; }

    std::array<size_t, 256> shift;
    shift.fill(nlen);
    for (size_t i = 0; i + 1 < nlen; ++i) {
        if (!has_wild || mask[i] != '?')
            shift[needle[i]] = nlen - 1 - i;
    }

    size_t i = start_offset;
    while (i + nlen <= hay_len) {
        if (alignment > 1 && (i % alignment) != 0) { i += (alignment - (i % alignment)); continue; }
        size_t j = nlen;
        while (j > 0) {
            --j;
            if (mask[j] == '?') continue;
            if (haystack[i + j] != needle[j]) break;
        }
        if (j == 0) {
            bool ok = true;
            for (size_t k = 0; k < nlen; ++k) if (mask[k] != '?' && haystack[i + k] != needle[k]) { ok = false; break; }
            if (ok) return i;
        }
        uint8_t next = haystack[i + nlen - 1];
        size_t s = shift[next];
        if (s == 0) s = 1;
        i += s;
    }
    return SIZE_MAX;
}

#endif // MASKED_BMH_H
