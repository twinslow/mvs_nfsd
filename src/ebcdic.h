/*
 * ebcdic.h - EBCDIC <-> ASCII translation for the MVS port.
 *
 * Uses IBM Code Page 037 (CP037), the US/Canada EBCDIC code page used
 * by MVS 3.8j.
 *
 * Key mappings relevant to PDS member names and JCL text:
 *
 *   EBCDIC  ASCII    EBCDIC  ASCII    EBCDIC  ASCII
 *   0x40    0x20 ' ' 0x7A    0x3A ':' 0xC1    0x41 'A'
 *   0x4B    0x2E '.' 0x7B    0x23 '#' ...
 *   0x4C    0x3C '<' 0x7C    0x40 '@' 0xC9    0x49 'I'
 *   0x4D    0x28 '(' 0x7D    0x27 ''' 0xD1    0x4A 'J'
 *   0x4E    0x2B '+' 0x7E    0x3D '=' ...
 *   0x4F    0x7C '|' 0x7F    0x22 '"' 0xD9    0x52 'R'
 *   0x50    0x26 '&' 0x81    0x61 'a' 0xE2    0x53 'S'
 *   0x5A    0x21 '!' ...                ...
 *   0x5B    0x24 '$' 0x89    0x69 'i' 0xE9    0x5A 'Z'
 *   0x5C    0x2A '*' 0x91    0x6A 'j' 0xF0    0x30 '0'
 *   0x5D    0x29 ')' ...                ...
 *   0x5E    0x3B ';' 0x99    0x72 'r' 0xF9    0x39 '9'
 *   0x5F    0x5E '^' 0xA2    0x73 's'
 *   0x60    0x2D '-' ...
 *   0x61    0x2F '/' 0xA9    0x7A 'z'
 *   0x6B    0x2C ',' 0xBD    0x5D ']'
 *   0x6C    0x25 '%' 0xAD    0x5B '['
 *   0x6D    0x5F '_' 0xC0    0x7B '{'
 *   0x6E    0x3E '>' 0xD0    0x7D '}'
 *   0x6F    0x3F '?' 0xA1    0x7E '~'
 *   0x79    0x60 '`' 0xE0    0x5C '\'
 *
 * EBCDIC NL (0x15) maps to ASCII LF (0x0A).
 * ASCII LF (0x0A) maps to EBCDIC NL (0x15).
 * EBCDIC CR (0x0D) maps to ASCII CR (0x0D).
 *
 * Unmapped EBCDIC bytes (Latin-1 extensions above 0x7F) are
 * translated to '?' (0x3F) in EBCDIC->ASCII direction.
 * Unmapped ASCII bytes below 0x20 (control chars) are translated
 * to EBCDIC 0x00 in ASCII->EBCDIC direction.
 */
 
#ifndef EBCDIC_H
#define EBCDIC_H
 
#include "types.h"
 
/* Short names for MVS compatibility */
#if defined(__MVS__)
#define ebcdic_to_ascii         ebcToAsc
#define ascii_to_ebcdic         ascToEbc
#define ebcdic_member_to_name   ebcMbToN
#define name_to_ebcdic_member   namToEmb
#endif /* __MVS__ */
 
/* ------------------------------------------------------------------- */
/* Translation tables (defined in ebcdic.c)                            */
/* ------------------------------------------------------------------- */
extern const uint8_t ebcdic_to_ascii_tab[256];
extern const uint8_t ascii_to_ebcdic_tab[256];
 
/* -------------------------------------------------------------------- */
/* Inline single-character translators                                  */
/* -------------------------------------------------------------------- */
 
/* Translate one EBCDIC byte to ASCII. */
static uint8_t ebcdic_to_ascii_c(uint8_t e)
{
    return ebcdic_to_ascii_tab[e];
}
 
/* Translate one ASCII byte to EBCDIC. */
static uint8_t ascii_to_ebcdic_c(uint8_t a)
{
    return ascii_to_ebcdic_tab[a];
}
 
/* -------------------------------------------------------------------- */
/* Buffer translators                                                   */
/* -------------------------------------------------------------------- */
 
/*
 * ebcdic_to_ascii: translate 'len' bytes from src (EBCDIC) to dst (ASCII).
 * dst and src may be the same buffer (in-place translation is safe).
 */
void ebcdic_to_ascii(uint8_t *dst, const uint8_t *src, size_t len);
 
/*
 * ascii_to_ebcdic: translate 'len' bytes from src (ASCII) to dst (EBCDIC).
 */
void ascii_to_ebcdic(uint8_t *dst, const uint8_t *src, size_t len);
 
/*
 * ebcdic_member_to_name: convert an 8-byte EBCDIC PDS member name
 * (space-padded, upper-case) to a NUL-terminated ASCII filename.
 *
 * MVS member names are 1-8 characters: A-Z, 0-9, @, #, $.
 * Trailing EBCDIC spaces (0x40) are stripped.
 *
 * dst must be at least 9 bytes.
 * Returns the length of the resulting ASCII name.
 */
int ebcdic_member_to_name(char *dst, const uint8_t *member8);
 
/*
 * name_to_ebcdic_member: convert a NUL-terminated ASCII filename to
 * an 8-byte EBCDIC PDS member name (upper-case, space-padded).
 *
 * Returns 0 if the name is valid (1-8 chars, valid member chars),
 * -1 if the name is too long or contains invalid characters.
 */
int name_to_ebcdic_member(uint8_t *member8, const char *name);
 

#endif /* EBCDIC_H */
