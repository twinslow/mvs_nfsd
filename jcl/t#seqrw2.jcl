//TONYWZ1  JOB (DINO),
//             'Test w+b open',
//             CLASS=A,COND=(0,LT),
//             MSGCLASS=X,
//             REGION=8M,TIME=1440,
//             MSGLEVEL=(1,1),
//             NOTIFY=TONYW
//********************************************************************
//*
//* Does a SINGLE "w+b"+DCB fopen create a scratch dataset AND then
//* support random fseek/fwrite/fread on the same handle?  If so the
//* spill design (doc/design_nfs_write.md Sec 8) can use one open
//* instead of the proven create("wb")-then-reopen("r+b") pair.
//*
//* PASS => rc 0, FAIL => rc 8 (see STDERR for the details).
//*
//********************************************************************
//TEST EXEC JCCCG
//COMPILE.SYSIN DD DATA,DLM=@@
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define TEST_FILE  "//DSN:&&SEQRW2"
#define BLKSZ      4096
#define NBLK       20        /* 20 * 4KB = 80KB, many blocks */
#define NRAND      50        /* random update iterations     */

static char block[BLKSZ];
static int  expected[NBLK];

/* Write 'value' as the first bytes of block 'b', at b's offset. */
static int put_block(FILE *fp, int b, int value) {
    long off = (long)b * BLKSZ;
    int  n;

    if (fseek(fp, off, SEEK_SET) != 0) {
        fprintf(stderr, "put_block: fseek to %ld failed\n", off);
        perror("fseek");
        return 8;
    }
    memset(block, 0, BLKSZ);
    memcpy(block, &value, sizeof(value));
    sprintf(block + sizeof(value), "block %d value %d", b, value);
    n = fwrite(block, 1, BLKSZ, fp);
    if (n != BLKSZ) {
        fprintf(stderr, "put_block %d: fwrite=%d want %d\n",
            b, n, BLKSZ);
        return 8;
    }
    return 0;
}

/* Read block 'b' and return its stored int value in *value. */
static int get_block(FILE *fp, int b, int *value) {
    long off = (long)b * BLKSZ;
    int  n;

    if (fseek(fp, off, SEEK_SET) != 0) {
        fprintf(stderr, "get_block: fseek to %ld failed\n", off);
        perror("fseek");
        return 8;
    }
    n = fread(block, 1, BLKSZ, fp);
    if (n != BLKSZ) {
        fprintf(stderr, "get_block %d: fread=%d want %d\n",
            b, n, BLKSZ);
        return 8;
    }
    memcpy(value, block, sizeof(*value));
    return 0;
}

int main(void) {
    FILE *fp;
    int   i, b, v, got;
    int   errors = 0;

    srand((unsigned)time(NULL));

    /* (1) One "w+b"+DCB open -- must CREATE the dataset. */
    fprintf(stderr, "opening %s w+b ...\n", TEST_FILE);
    fp = fopen(TEST_FILE,
               "w+b,pri=15,sec=15,rlse,unit=sysda,"
               "dsorg=ps,recfm=fb,blksize=4096,lrecl=4096");
    if (!fp) {
        perror("fopen w+b failed (create not supported?)");
        return 8;
    }
    fprintf(stderr, "open OK -- dataset created\n");

    /* (2) Sequential fwrite grows the file to NBLK blocks. */
    for (i = 0; i < NBLK; i++) {
        if (put_block(fp, i, i) != 0)
            errors++;
        expected[i] = i;
    }
    fprintf(stderr, "wrote %d blocks sequentially\n", NBLK);

    /* (3) Random write THEN read back on the SAME handle. */
    for (i = 0; i < NRAND; i++) {
        b = rand() % NBLK;
        v = 1000 + i;
        if (put_block(fp, b, v) != 0) {
            errors++;
            continue;
        }
        expected[b] = v;
        if (get_block(fp, b, &got) != 0) {
            errors++;
            continue;
        }
        if (got != v) {
            fprintf(stderr, "readback bad: blk %d wrote %d got %d\n",
                b, v, got);
            errors++;
        }
    }
    fprintf(stderr, "did %d random write/read ops\n", NRAND);

    /* (3b) Byte-granular (non-block-aligned) seek+write+read, which
       the spill code needs for arbitrary NFS write offsets. */
    {
        long boff = 100;
        char msg[32];
        char chk[32];

        memset(msg, 0, sizeof(msg));
        strcpy(msg, "byte-offset-100");
        if (fseek(fp, boff, SEEK_SET) != 0 ||
            fwrite(msg, 1, sizeof(msg), fp) != sizeof(msg)) {
            perror("byte write failed");
            errors++;
        } else if (fseek(fp, boff, SEEK_SET) != 0 ||
                   fread(chk, 1, sizeof(msg), fp) != sizeof(msg)) {
            perror("byte read failed");
            errors++;
        } else if (memcmp(msg, chk, sizeof(msg)) != 0) {
            fprintf(stderr, "byte-offset readback mismatch\n");
            errors++;
        } else {
            fprintf(stderr, "byte-offset 100 seek/write/read OK\n");
        }
    }

    /* (4) Final pass: every block must hold its expected value. */
    for (i = 0; i < NBLK; i++) {
        if (get_block(fp, i, &got) != 0) {
            errors++;
            continue;
        }
        if (got != expected[i]) {
            fprintf(stderr, "final bad: blk %d want %d got %d\n",
                i, expected[i], got);
            errors++;
        }
    }

    fclose(fp);

    if (errors == 0) {
        fprintf(stderr, "\nPASS: single w+b open created + random"
            " write/read on one handle\n");
        return 0;
    }
    fprintf(stderr, "\nFAIL: %d error(s)\n", errors);
    return 8;
}

@@
//GO.STDERR DD  SYSOUT=*,DCB=(RECFM=F,BLKSIZE=133)
//
