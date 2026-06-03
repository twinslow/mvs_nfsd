#include <stdio.h>
#include <process.h>   /* for Sleep() */

#include "types.h"

#define getcib GETCIB
extern int getcib(void *cibdata, size_t cibdatalen, int *length_data);


int main(argc, argv) {
    uint8_t cibdata[9];
    int length_data = 0;
    int rc;
    int loopcount = 0;

    while (1) {
        rc = getcib(cibdata, sizeof(cibdata) - 1, &length_data);
        //fprintf(stderr, "getcib returned rc=%d, length_data=%d\n", rc, length_data);
        if (rc < 0) {
            fprintf(stderr, "getcib failed with rc=%d\n", rc);
            break;
        }

        if ( rc == 1 && length_data > 0 ) {
            if ( length_data > 0 )
                cibdata[length_data] = '\0';  /* NUL-terminate for printing as string */
            fprintf(stderr, "Modify command : %s\n", cibdata);
        } else if (rc == 2 ) {
            fprintf(stderr, "Stop command\n");
            break;
        }
        Sleep(200);  /* Sleep for a bit before polling again */
        //loopcount++;
        //fprintf(stderr, "   loopcount: %d\n", loopcount);
        //if ( loopcount > 120 ) {
        //    fprintf(stderr, "Exiting loop\n");
        //    break;
        //}
    }

    return 0;
}