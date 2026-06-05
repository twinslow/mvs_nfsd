
/*************************************************************************************
 *                                                                                   *
 * 2026/06/05 - Anthony R. Winslow.                                                  *
 *                                                                                   *
 * This source module is released as public domain without warranty.                 *
 *                                                                                   *
 *************************************************************************************/

#include <sockets.h>
#include <stdio.h>
#include <stdlib.h>



int is_socket_for_port(
    int     *port_list,
    SOCKET  socket_id,
    char    *ip_address,
    int     *port_number
) 
{
    struct  herc_in_addr    herc_sock_info;                                              
    struct  sockaddr        sock_addr;
    struct  sockaddr_in    *sock_addr_in;                                                
    int     retval;
    int     port;
    int     size_sock_addr;
    int     i;

    size_sock_addr = sizeof(sock_addr);
    retval = getsockname(socket_id, &sock_addr, &size_sock_addr);
    if (retval == SOCKET_ERROR) {
        return -1;
    }
    
    sock_addr_in = (struct sockaddr_in *)&sock_addr;

    /* Copy the IP address over to the herc_in_addr structure */
    herc_sock_info.S_un.S_addr = sock_addr_in->sin_addr.s_addr;

    /* Get the port */
    port = ntohs(sock_addr_in->sin_port);

    /* Check if the port is in the list */
    for (i = 0; port_list[i] >= 0; i++) {
        if (port_list[i] == port) {
            /* Found in list - return formatted IP address and port */
            *port_number = port;
            snprintf(ip_address, 16, "%d.%d.%d.%d",
                herc_sock_info.S_un.S_un_b.s_b1,
                herc_sock_info.S_un.S_un_b.s_b2,
                herc_sock_info.S_un.S_un_b.s_b3,
                herc_sock_info.S_un.S_un_b.s_b4
            );
            return 1;
        }
    }
    /* Port not found in list */
    return 0;
}

int main (int argc, char *argv[]) {
     
    int         port_list[51]; /* List of ports to check, terminated by -1 */
    int         i;
    int         port_count = 0;
    char        ip_address[16];
    int         port_number;
    SOCKET      socket_id;
    int         error_count = 0;
    int         query_result;
    int         close_result;

    for (i = 1; i < argc && port_count < 50; i++) {
        int port = atoi(argv[i]);
        if (port > 0 && port <= 65535) {
            port_list[port_count++] = port;
        } else {
            fprintf(stderr, "Invalid port number: %s\n", argv[i]);
        }
    }
    port_list[port_count] = -1; /* Terminate the list */
    if (port_count == 0) {
        fprintf(stderr, "No valid ports provided. Usage: ressock <port1> <port2> ...\n");
        return 8;
    } else if ( port_count == 50 && argc > 51) {
        /* Note that MVS parm field length limit doesn't allow this port limit to be reached */
        fprintf(stderr, "Warning: Maximum of 50 ports can be checked. Extra ports will be ignored.\n");
    }

    for (socket_id = 1; socket_id < 1024; socket_id++) {
        query_result = is_socket_for_port(port_list, socket_id, ip_address, &port_number);
        if (query_result == 1) {
            close_result = closesocket(socket_id);
            printf("Socket %d connected to %s:%d -- %s\n", 
                socket_id, ip_address, port_number, 
                close_result == 0 ? "Closed" : "Failed to close" );
            if (close_result != 0) {
                error_count++;
            }
        }
    }

    if (error_count > 0) {
        fprintf(stderr, "Encountered %d errors while closing sockets.\n", error_count);
        return 8;
    }

    return 0;

}

