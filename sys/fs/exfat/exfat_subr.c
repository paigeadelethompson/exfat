/* Fix error messages */
    if (bootverbose)
        printf("exfat: [exfat_utf16_to_utf8] converting UTF-16 string to UTF-8\n");

    if (bootverbose)
        printf("exfat: [exfat_utf8_to_utf16] converting UTF-8 string to UTF-16\n");

    if (bootverbose)
        printf("exfat: [exfat_toupper] converting character to uppercase\n");

    if (bootverbose)
        printf("exfat: [exfat_tolower] converting character to lowercase\n");

    if (bootverbose)
        printf("exfat: [exfat_name_hash] calculating hash for name\n");

    if (bootverbose)
        printf("exfat: [exfat_dirent_cmp] comparing directory entries\n");

    if (bootverbose)
        printf("exfat: [exfat_scan_directory] scanning directory\n");

    if (bootverbose)
        printf("exfat: [exfat_scan_directory] reached end of directory\n");

    if (bootverbose)
        printf("exfat: [exfat_scan_directory] error reading directory: %d\n", error); 