# ExFAT Implementation for FreeBSD

**⚠️ IMPORTANT NOTICE**: This is an experimental implementation of ExFAT for FreeBSD. Before using this code, please be aware of and review the following:

1. Patent Status: There are relevant Microsoft patents covering ExFAT functionality:
   - File name hash for efficient file lookup
   - Contiguous file storage without using FAT
   
   While Microsoft has contributed ExFAT to OIN (Open Invention Network), please review [issue #1](https://github.com/paigeadelethompson/exfat/issues/1) for ongoing discussion about patent implications.

2. Development Status: This implementation is a work in progress and should not be considered production-ready.

## Building

### Prerequisites
- FreeBSD 14.0 or later
- Basic development tools (compiler, make)

### Build Instructions

1. Clone the repository:
```
bash
git clone https://github.com/paigeadelethompson/exfat.git
cd exfat
```

2. Build the newfs_exfat tool:
```
bash
make
```


This will build:
- mount_exfat: ExFAT filesystem mount utility
- newfs_exfat: Create new ExFAT filesystem
- fsck_exfat: Check and repair ExFAT filesystem
- defrag_exfat: Defragment ExFAT filesystem

### Installation
```
bash
sudo make install
```



## Features

- Basic read/write support
- Mount/unmount functionality
- Filesystem creation
- Filesystem checking
- Defragmentation support

## Limitations

- Experimental implementation
- Not yet thoroughly tested
- Some features may be patent-encumbered (see above notice)

## Contributing

Please review open issues and ongoing discussions before contributing (or even using this code.) In particular, note the patent-related discussions in [issue #1](https://github.com/paigeadelethompson/exfat/issues/1).

## License
**⚠️ We're not really sure what this is yet, but the hope is ** BSD 2-Clause License. See LICENSE file for details.

## References

- [Microsoft ExFAT Licensing](https://www.microsoft.com/en-us/legal/intellectualproperty/mtl/paragon)
- [ExFAT Patents Discussion](https://github.com/paigeadelethompson/exfat/issues/1)

## Disclaimer

This implementation is provided as-is, without warranty of any kind. Users should perform their own legal due diligence regarding patent implications before using this code.