//TONYWZ1  JOB (DINO),
//             'Test membr write fb3',
//             CLASS=A,COND=(0,LT),
//             MSGCLASS=X,
//             REGION=8M,TIME=1440,
//             MSGLEVEL=(1,1),
//             NOTIFY=TONYW
//********************************************************************
//*
//* Testing random file read write to sequentila FB datsset
//*
//********************************************************************
//TEST EXEC JCCCG
//COMPILE.SYSIN DD DATA,DLM=@@
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <io.h>
#include <time.h>

#define TEST_FILE_NAME "//DSN:TEMP.TESTPROJ.SEQFB"
#define FILE_BLOCK_SIZE      4096
#define FILE_BLOCK_COUNT     12*15*3

int format_file() {
    FILE *ofh;
    char *file_name = TEST_FILE_NAME;
    char block[FILE_BLOCK_SIZE];
    int bwritten, i;

    memset(block, 0, FILE_BLOCK_SIZE);
    ofh = fopen(file_name, "wb");
    if ( !ofh ) {
        perror("Open failed");
        return 8;
    }
    
    for ( i = 0; i < FILE_BLOCK_COUNT; i++) {   
        memcpy(block, &i, sizeof(i));    
        bwritten = fwrite(block, 1, FILE_BLOCK_SIZE, ofh);
        fprintf(stderr, "Written block %d of %d bytes\n",
            i, bwritten);
    }

    fclose(ofh); 

    fprintf(stderr, "Format file complete");

    return 0;
}

int block_read(FILE *fh, int block_num, char *buff) {

    int bread;
    int rc;
    int offset;

    offset = FILE_BLOCK_SIZE * block_num;
    rc = fseek(fh, offset, SEEK_SET);
    if ( rc < 0 ) {
        perror("fseek failed");
        return 8;
    }

    fprintf(stderr, "block_read: Reading block_num %d offset %d\n",
        block_num, offset);

    bread = fread(buff, 1, FILE_BLOCK_SIZE, fh);
    if (bread < 0) {
        perror("block read error");
        return 8;
    } else if (bread != FILE_BLOCK_SIZE) {
        fprintf(stderr, 
            "Error: unexpected read size from block %d, size %d\n",
            block_num, bread);
    }

    return 0;
}

int block_write(FILE *fh, int block_num, char *buff) {

    int bwritten;
    int rc;
    int offset;

    offset = FILE_BLOCK_SIZE * block_num;
    rc = fseek(fh, offset, SEEK_SET);
    if ( rc < 0 ) {
        perror("fseek failed");
        return 8;
    }

    bwritten = fwrite(buff, 1, FILE_BLOCK_SIZE, fh);
    if (bwritten < 0) {
        perror("block write error");
        return 8;
    } else if (bwritten != FILE_BLOCK_SIZE) {
        fprintf(stderr, 
            "Error: unexpected write size for block %d, size %d\n",
            block_num, bwritten);
    }

    return 0;
}

int main(int argc, char **argv) {

    int        rc = 0;
    FILE      *sf;
    char      *file_name = TEST_FILE_NAME;
    char       block[FILE_BLOCK_SIZE];
    int        i;
    int        block_num;

    //rc = format_file();
    //return rc;
    srand(time(NULL));

    sf = fopen(file_name, "r+");

    for (i = 0; i < 1; i++) {
        block_num = rand() % FILE_BLOCK_COUNT;

        fprintf(stderr, "Reading block %d\n", block_num);
        rc = block_read(sf, block_num, block);
        if ( rc > 0 )
            break;

        sprintf(block + 8, "Random update number %d on block %d      ", 
          i, block_num);

        fprintf(stderr, "Writing block %d\n", block_num);
        rc = block_write(sf, block_num, block);
        if ( rc > 0 )
            break;
    }

    fclose(sf);

    return rc;
}

@@
//GO.STDERR DD  SYSOUT=*,DCB=(RECFM=F,BLKSIZE=133)
//
