#ifndef ASMUTILS_H
#define ASMUTILS_H

#include "types.h"

#define getcib          GETCIB
#define mvs_dynalloc    MVSDALC 
#define mvs_stow        MVSSTOW
#define mvs_enq         MVSENQ

#define MVS_DYNALLOC_REQ_ALLOC      1
#define MVS_DYNALLOC_REQ_UNALLOC    2
#define MVS_DYNALLOC_OPT_FREECLOSE  0x01

#define MVS_ENQ_REQ_ENQ     1
#define MVS_ENQ_REQ_DEQ     2
#define MVS_ENQ_REQ_ENQTEST 3

#define MVS_ENQ_OPT_EXC     0x01
#define MVS_ENQ_OPT_SHR     0x00

extern int getcib(
    void       *cibdata, 
    size_t      cibdatalen, 
    int        *length_data);

extern int mvs_dynalloc(
    uint8_t     request_type,
    uint8_t     options,
    const char *dsname,
    const char *member,
    char       *ddname     /* Area char[8] to receive ddname of allocated dataset */);

extern int mvs_stow(
    const char *ddname,
    const char *member,
    void       *userdata,
    int         user_data_length_bytes);

extern int mvs_enq(
    uint8_t     request_type,
    uint8_t     options,
    const char *qname,
    const char *rname);

#endif /* ASMUTILS_H */