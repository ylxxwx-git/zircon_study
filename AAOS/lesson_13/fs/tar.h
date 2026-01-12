#ifndef _TAR_H
#define _TAR_H

#include <stdint.h>

struct tar_header {
    char name[100];
    char mode[8];
    char uid[8];
    char gid[8];
    char size[12];
    char mtime[12];
    char chksum[8];
    char typeflag;
    char linkname[100];
    char magic[6];
    char version[2];
    char uname[32];
    char gname[32];
    char devmajor[8];
    char devminor[8];
    char prefix[155];
    char pad[12];
};

void tar_init(void);
int tar_find(const char *name, uint64_t *sector, uint32_t *size);
int tar_read(uint64_t sector, uint32_t size, void *buffer);

#endif
