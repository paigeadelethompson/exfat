/*-
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * Copyright (c) 1994
 *      The Regents of the University of California.  All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 * 3. Neither the name of the University nor the names of its contributors
 *    may be used to endorse or promote products derived from this software
 *    without specific prior written permission.
 */

#include <sys/param.h>

#include <err.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>

#include "mntopts.h"

int
getmntopts(const char *options, const struct mntopt *m0, int *flagp,
    int *altflagp)
{
    const struct mntopt *m;
    int negative;
    char *opt, *optbuf, *p;
    int *thisflagp;

    /* Copy option string, since it is about to be torn asunder. */
    if ((optbuf = strdup(options)) == NULL)
        err(1, NULL);

    for (opt = optbuf; (opt = strtok(opt, ",")) != NULL; opt = NULL) {
        /* Check for "no" prefix. */
        if (opt[0] == 'n' && opt[1] == 'o') {
            negative = 1;
            opt += 2;
        } else
            negative = 0;

        /*
         * for options with assignments in them (ie. quotas)
         * ignore the assignment as it's handled elsewhere
         */
        p = strchr(opt, '=');
        if (p)
            *p = '\0';

        /* Scan option table. */
        for (m = m0; m->name != NULL; m++) {
            if (strcasecmp(opt, m->name) != 0)
                continue;
            thisflagp = m->inverse ? altflagp : flagp;
            if (negative)
                *thisflagp &= ~m->inverse;
            else
                *thisflagp |= m->flags;
            break;
        }
    }

    free(optbuf);
    return (0);
} 