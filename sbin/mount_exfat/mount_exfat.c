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

#include <sys/param.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <sys/sysctl.h>

#include <err.h>
#include <errno.h>
#include <fcntl.h>
#include <sys/mntopts.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sysexits.h>
#include <unistd.h>

#include "mount_exfat.h"

static struct mntopt mopts[] = {
    MOPT_STDOPTS,
    MOPT_FORCE,
    MOPT_SYNC,
    MOPT_UPDATE,
    MOPT_NOATIME,
    { "uid", 0, EXFAT_MNT_UID, 0 },
    { "gid", 0, EXFAT_MNT_GID, 0 },
    { "mask", 0, EXFAT_MNT_MASK, 0 },
    { "dmask", 0, EXFAT_MNT_DMASK, 0 },
    { "fmask", 0, EXFAT_MNT_FMASK, 0 },
    { NULL }
};

static void
usage(void)
{
    fprintf(stderr,
        "usage: mount_exfat [-o options] special node\n"
        "    -o sync         Mount filesystem synchronously\n"
        "    -o noatime      Do not update access times\n"
        "    -o uid=value    Set owner of files\n"
        "    -o gid=value    Set group of files\n"
        "    -o mask=value   Set mask for all files\n"
        "    -o dmask=value  Set mask for directories\n"
        "    -o fmask=value  Set mask for regular files\n");
    exit(EX_USAGE);
}

static int
validate_mount_options(struct exfat_args *args)
{
    /* Validate UID/GID */
    if ((args->flags & EXFAT_MNT_UID) && args->uid == (uid_t)-1) {
        warnx("Invalid uid value");
        return -1;
    }
    if ((args->flags & EXFAT_MNT_GID) && args->gid == (gid_t)-1) {
        warnx("Invalid gid value");
        return -1;
    }

    /* Validate masks */
    if ((args->flags & EXFAT_MNT_MASK) && (args->mask & ~07777)) {
        warnx("Invalid mask value");
        return -1;
    }
    if ((args->flags & EXFAT_MNT_DMASK) && (args->dmask & ~07777)) {
        warnx("Invalid dmask value");
        return -1;
    }
    if ((args->flags & EXFAT_MNT_FMASK) && (args->fmask & ~07777)) {
        warnx("Invalid fmask value");
        return -1;
    }

    return 0;
}

static int
validate_device(const char *dev)
{
    struct stat sb;

    if (stat(dev, &sb) == -1) {
        warn("Cannot stat %s", dev);
        return -1;
    }

    if (!S_ISBLK(sb.st_mode) && !S_ISCHR(sb.st_mode)) {
        warnx("%s is not a block or character device", dev);
        return -1;
    }

    return 0;
}

static int
validate_mountpoint(const char *dir)
{
    struct stat sb;

    if (stat(dir, &sb) == -1) {
        warn("Cannot stat %s", dir);
        return -1;
    }

    if (!S_ISDIR(sb.st_mode)) {
        warnx("%s is not a directory", dir);
        return -1;
    }

    return 0;
}

int
main(int argc, char *argv[])
{
    struct exfat_args args;
    char *dev, *dir;
    int ch, mntflags;
    mode_t mask = 0, dmask = 0, fmask = 0;
    uid_t uid = 0;
    gid_t gid = 0;
    int error = 0;

    memset(&args, 0, sizeof(args));
    mntflags = 0;

    while ((ch = getopt(argc, argv, "o:")) != -1) {
        switch (ch) {
        case 'o':
            getmntopts(optarg, mopts, &mntflags, &args.flags);
            if (args.flags & EXFAT_MNT_UID)
                uid = (uid_t)strtoul(getval("uid", optarg), NULL, 0);
            if (args.flags & EXFAT_MNT_GID)
                gid = (gid_t)strtoul(getval("gid", optarg), NULL, 0);
            if (args.flags & EXFAT_MNT_MASK)
                mask = (mode_t)strtoul(getval("mask", optarg), NULL, 8);
            if (args.flags & EXFAT_MNT_DMASK)
                dmask = (mode_t)strtoul(getval("dmask", optarg), NULL, 8);
            if (args.flags & EXFAT_MNT_FMASK)
                fmask = (mode_t)strtoul(getval("fmask", optarg), NULL, 8);
            break;
        default:
            usage();
        }
    }
    argc -= optind;
    argv += optind;

    if (argc != 2)
        usage();

    dev = argv[0];
    dir = argv[1];

    /* Set up mount arguments */
    args.fspec = dev;
    args.uid = uid;
    args.gid = gid;
    args.mask = mask;
    args.dmask = dmask;
    args.fmask = fmask;

    /* Load exfat kernel module if needed */
    if (kldload("exfat") == -1 && errno != EEXIST)
        warn("Cannot load 'exfat' kernel module");

    /* Validate mount options */
    if (validate_mount_options(&args) != 0)
        return EX_USAGE;

    /* Perform the mount */
    error = mount("exfat", dir, mntflags, &args);
    if (error) {
        if (errno == ENODEV)
            errx(EX_OSERR, "ExFAT filesystem support not available");
        else
            err(EX_OSERR, "Cannot mount %s on %s", dev, dir);
    }

    return 0;
} 