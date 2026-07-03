//TONYWZ1  JOB (DINO),
//             'Test membr write fb3',
//             CLASS=A,COND=(0,LT),
//             MSGCLASS=X,
//             REGION=8M,TIME=1440,
//             MSGLEVEL=(1,1),
//             NOTIFY=TONYW
//********************************************************************
//*
//* Testing file write to PDS members 3
//*
//********************************************************************
//TEST EXEC JCCCG
//COMPILE.SYSIN DD DATA,DLM=@@
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <io.h>


char *create_file_content(int num_lines, int *file_mem_size) {

    char *file_mem;
    char *line_pattern = "T2 - This is line %04d\n";
    char  buff[256];
    int   linelen;
    char *buff_out;

    int   i;
    sprintf(buff, line_pattern, 1);
    linelen = strlen(buff);

    file_mem = malloc(num_lines * (linelen + 1));
    buff_out = file_mem;  
    for ( i = 1; i <= num_lines; i++ ) {
        buff_out += sprintf(buff_out, line_pattern, i);
    }

    *file_mem_size = (buff_out - file_mem);

    return file_mem;
}

int main(int argc, char **argv) {

    int        rc = 0;
    FILE      *ofh;
    char      *out_file_name = "//DSN:TEMP.TESTPROJ.OUTFB(TEST3)";
    size_t     bwritten;
    char       wbuff[256];
    int        i;
    char      *file_content;
    int        content_size;
    char      *wstart;
    int        wlen;
    int        max_write_size = 256;
    int        remaining; 
    char      *mode;
    fpos_t     write_fpos;

    file_content = create_file_content(30, &content_size);


    remaining = content_size;

    i = 0;
    for ( wstart = file_content; 
          wstart < file_content + content_size; ) {

        wlen = remaining < max_write_size ? remaining : max_write_size;

        fprintf(stderr, "Writing from 0x%08X, length %d (%d,%d)\n",
            wstart, wlen, remaining, max_write_size);

        mode = i == 0 ? "wt" : "at";
        ofh = fopen(out_file_name, mode);
        if ( i > 0 ) {
            rc = fsetpos(ofh, &write_fpos);
            fprintf(stderr, "      fsetpos rc = %d", rc);
        }
        bwritten = fwrite(wstart, 1, wlen, ofh);
        fprintf(stderr, "      fwrite bwritten = %d\n",bwritten);
        rc = fgetpos(ofh, &write_fpos);
        fprintf(stderr, "      fgetpos rc = %d\n",rc);
        fclose(ofh);

        wstart += bwritten;
        remaining -= bwritten;
        i++;
        if ( i > 100 )
            break;
    } 



    return rc;
}

@@
//GO.STDERR DD  SYSOUT=*,DCB=(RECFM=F,BLKSIZE=133)
//
