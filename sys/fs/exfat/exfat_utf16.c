#include <sys/param.h>
#include <sys/systm.h>
#include <sys/kernel.h>

#include "exfat_utf16.h"

/*
 * Convert UTF-8 to UTF-16
 */
int
exfat_utf8_to_utf16(const char *utf8, uint16_t *utf16, size_t outlen, size_t *outused)
{
    size_t i = 0;
    uint32_t codepoint;
    const uint8_t *s = (const uint8_t *)utf8;

    *outused = 0;

    while (*s && *outused < outlen) {
        /* Decode UTF-8 sequence */
        if ((*s & 0x80) == 0) {
            /* 1-byte sequence */
            codepoint = *s++;
        } else if ((*s & 0xE0) == 0xC0) {
            /* 2-byte sequence */
            if ((s[1] & 0xC0) != 0x80)
                return EILSEQ;
            codepoint = ((*s & 0x1F) << 6) | (s[1] & 0x3F);
            s += 2;
            if (codepoint < 0x80)  /* Overlong encoding */
                return EILSEQ;
        } else if ((*s & 0xF0) == 0xE0) {
            /* 3-byte sequence */
            if ((s[1] & 0xC0) != 0x80 || (s[2] & 0xC0) != 0x80)
                return EILSEQ;
            codepoint = ((*s & 0x0F) << 12) | ((s[1] & 0x3F) << 6) | (s[2] & 0x3F);
            s += 3;
            if (codepoint < 0x800)  /* Overlong encoding */
                return EILSEQ;
        } else {
            /* Invalid or unsupported sequence */
            return EILSEQ;
        }

        /* Encode as UTF-16 */
        if (codepoint < 0x10000) {
            utf16[(*outused)++] = htole16(codepoint);
        } else if (codepoint <= 0x10FFFF) {
            /* Surrogate pair */
            if (*outused + 1 >= outlen)
                return E2BIG;
            codepoint -= 0x10000;
            utf16[(*outused)++] = htole16(0xD800 | (codepoint >> 10));
            utf16[(*outused)++] = htole16(0xDC00 | (codepoint & 0x3FF));
        } else {
            return EILSEQ;
        }
    }

    if (*s)
        return E2BIG;

    return 0;
}

/*
 * Convert UTF-16 to UTF-8
 */
int
exfat_utf16_to_utf8(const uint16_t *utf16, size_t utf16len, char *utf8, size_t outlen, size_t *outused)
{
    size_t i = 0;
    uint32_t codepoint;
    uint8_t *out = (uint8_t *)utf8;
    *outused = 0;

    while (i < utf16len && *outused < outlen) {
        /* Decode UTF-16 */
        codepoint = le16toh(utf16[i++]);
        if (codepoint >= 0xD800 && codepoint <= 0xDBFF) {
            /* High surrogate */
            if (i >= utf16len)
                return EILSEQ;
            uint16_t low = le16toh(utf16[i++]);
            if (low < 0xDC00 || low > 0xDFFF)
                return EILSEQ;
            codepoint = 0x10000 + ((codepoint - 0xD800) << 10) + (low - 0xDC00);
        } else if (codepoint >= 0xDC00 && codepoint <= 0xDFFF) {
            /* Unexpected low surrogate */
            return EILSEQ;
        }

        /* Encode as UTF-8 */
        if (codepoint < 0x80) {
            out[(*outused)++] = codepoint;
        } else if (codepoint < 0x800) {
            if (*outused + 2 > outlen)
                return E2BIG;
            out[(*outused)++] = 0xC0 | (codepoint >> 6);
            out[(*outused)++] = 0x80 | (codepoint & 0x3F);
        } else if (codepoint < 0x10000) {
            if (*outused + 3 > outlen)
                return E2BIG;
            out[(*outused)++] = 0xE0 | (codepoint >> 12);
            out[(*outused)++] = 0x80 | ((codepoint >> 6) & 0x3F);
            out[(*outused)++] = 0x80 | (codepoint & 0x3F);
        } else {
            if (*outused + 4 > outlen)
                return E2BIG;
            out[(*outused)++] = 0xF0 | (codepoint >> 18);
            out[(*outused)++] = 0x80 | ((codepoint >> 12) & 0x3F);
            out[(*outused)++] = 0x80 | ((codepoint >> 6) & 0x3F);
            out[(*outused)++] = 0x80 | (codepoint & 0x3F);
        }
    }

    if (i < utf16len)
        return E2BIG;

    return 0;
} 