#include <stdio.h>
#include <ctype.h>

#include "ebcdic.h"

/*
 * Writes a formatted hex dump of buf/len to fp, preceded by an optional
 * header line. Each line shows the offset, 16 bytes of hex data (grouped
 * in fours), the bytes translated to ASCII, and the raw (native EBCDIC)
 * bytes.
 */
void hexdump(FILE *fp, const char *header, const void *buf, size_t len)
{
    const unsigned char *p = (const unsigned char *)buf;
    size_t offset;
    size_t i, n, hpos, apos, epos;
    char hexbuf[40];
    char asciibuf[21];
    char ebcdicbuf[21];
    unsigned char c;
    char a;

    if (header != NULL && *header != '\0')
        fprintf(fp, "%s\n", header);

    fprintf(fp, "Offset    Hexadecimal data                       "
                 "ASCII characters       EBCDIC characters\n");

    for (offset = 0; offset < len; offset += 16) {

        n = (len - offset < 16) ? (len - offset) : 16;
        hpos = 0;
        apos = 0;
        epos = 0;

        for (i = 0; i < 16; i++) {
            if (i < n) {
                c = p[offset + i];
                hpos += sprintf(hexbuf + hpos, "%02X", c);
                a = ascii_to_ebcdic_c(c);
                asciibuf[apos++] = isprint((unsigned char)a) ? (char)a : '.';
                ebcdicbuf[epos++] = isprint((unsigned char)c) ? (char)c : '.';
            } else {
                hexbuf[hpos++] = ' ';
                hexbuf[hpos++] = ' ';
            }

            if ((i % 4) == 3 && i != 15) {
                hexbuf[hpos++] = ' ';
                asciibuf[apos++] = ' ';
                ebcdicbuf[epos++] = ' ';
            }
        }

        hexbuf[hpos] = '\0';
        asciibuf[apos] = '\0';
        ebcdicbuf[epos] = '\0';

        fprintf(fp, "%08lX  %-35s   \"%-19s\"  \"%-19s\"\n",
                (unsigned long)offset, hexbuf, asciibuf, ebcdicbuf);
    }
}
