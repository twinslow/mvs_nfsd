#ifndef HEXDUMP_H
#define HEXDUMP_H

#include <stdio.h>

extern void hexdump(FILE *fp, const char *header, const void *buf, size_t len);

#endif /* HEXDUMP_H */