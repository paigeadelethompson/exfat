#ifndef _FS_EXFAT_UTF16_H_
#define _FS_EXFAT_UTF16_H_

#include <sys/param.h>
#include <sys/systm.h>

/* UTF-16 conversion functions */
int exfat_utf8_to_utf16(const char *utf8, uint16_t *utf16, size_t outlen, size_t *outused);
int exfat_utf16_to_utf8(const uint16_t *utf16, size_t utf16len, char *utf8, size_t outlen, size_t *outused);

#endif /* _FS_EXFAT_UTF16_H_ */ 