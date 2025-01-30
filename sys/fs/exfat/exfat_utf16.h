/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2024 The FreeBSD Foundation
 *
 * This software was developed by Paige A. Thompson (Ravenhammer Research.)
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 */
 
#ifndef _FS_EXFAT_UTF16_H_
#define _FS_EXFAT_UTF16_H_

#include <sys/param.h>
#include <sys/systm.h>

/* UTF-16 conversion functions */
int exfat_utf8_to_utf16(const char *utf8, uint16_t *utf16, size_t outlen, size_t *outused);
int exfat_utf16_to_utf8(const uint16_t *utf16, size_t utf16len, char *utf8, size_t outlen, size_t *outused);

#endif /* _FS_EXFAT_UTF16_H_ */ 