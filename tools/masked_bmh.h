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

    // find right-most non-wildcard position (anchor)
    ssize_t anchor = -1;
    for (ssize_t i = static_cast<ssize_t>(nlen) - 1; i >= 0; --i) {
        if (mask[i] != '?') { anchor = i; break; }
    }

    // if pattern is all-wildcards, any aligned position is a match
    if (anchor < 0) {
        size_t i = start_offset;
        while (i + nlen <= hay_len) {
            if (alignment == 1 || (i % alignment) == 0) return i;
            ++i;
        }
        return SIZE_MAX;
    }

    std::array<size_t, 256> shift;
    // default shift: move past the anchor
    shift.fill(nlen - static_cast<size_t>(anchor));
    for (size_t i = 0; i < static_cast<size_t>(anchor); ++i) {
        if (mask[i] != '?') {
            shift[needle[i]] = static_cast<size_t>(anchor) - i;
        }
    }

    size_t i = start_offset;
    while (i + nlen <= hay_len) {
        if (alignment > 1 && (i % alignment) != 0) { i += (alignment - (i % alignment)); continue; }

        // quick check at the anchor position
        if (haystack[i + anchor] != needle[anchor]) {
            uint8_t next = haystack[i + anchor];
            size_t s = shift[next];
            if (s == 0) s = 1;
            i += s;
            continue;
        }

        // verify remaining non-wild bytes
        bool ok = true;
        for (size_t k = 0; k < nlen; ++k) {
            if (mask[k] == '?') continue;
            if (haystack[i + k] != needle[k]) { ok = false; break; }
        }
        if (ok) return i;

        // advance by shift based on the anchor byte
        uint8_t next = haystack[i + anchor];
        size_t s = shift[next];
        if (s == 0) s = 1;
        i += s;
    }
    return SIZE_MAX;
}

#endif // MASKED_BMH_H
