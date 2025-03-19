#include <stdlib.h>
#include <unistd.h>
#include <stdio.h>
#include <string.h>
#include <signal.h>
#include <pthread.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <errno.h>


#include "pbx.h"
#include "server.h"
#include "debug.h"

static void terminate(int status);
static void terminate_handler(int signum);
volatile sig_atomic_t shutdown_flag = 0;
int server_fd = -1;


/*
 * "PBX" telephone exchange simulation.
 *
 * Usage: pbx <port>
 */
int main(int argc, char* argv[]) {
    int opt;
    char *port_str = NULL;
    int port;

    // option processing
    while ((opt = getopt(argc, argv, "p:")) != -1) {
        switch (opt) {
            case 'p':
                port_str = optarg;
                break;
            default:
                fprintf(stderr, "Usage: %s -p <port>\n", argv[0]);
                exit(EXIT_FAILURE);
        }
    }

    if (port_str == NULL) {
        fprintf(stderr, "Usage: %s -p <port>\n", argv[0]);
        exit(EXIT_FAILURE);
    }

    port = atoi(port_str);
    if (port <= 0 || port > 65535) {
        fprintf(stderr, "Invalid port number\n");
        exit(EXIT_FAILURE);
    }

    // perform required initialization of the PBX module.
    debug("Initializing PBX...");
    pbx = pbx_init();
    if (pbx == NULL) {
        fprintf(stderr, "Failed to initialize PBX\n");
        exit(EXIT_FAILURE);
    }

    // install SIGHUP handler
    struct sigaction sa;
    sa.sa_handler = terminate_handler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;

    if (sigaction(SIGHUP, &sa, NULL) == -1) {
        perror("sigaction");
        terminate(EXIT_FAILURE);
    }

    // set up the server socket
    server_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (server_fd == -1) {
        perror("socket");
        terminate(EXIT_FAILURE);
    }

    int optval = 1;
    if (setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &optval, sizeof(optval)) == -1) {
        perror("setsockopt");
        terminate(EXIT_FAILURE);
    }

    struct sockaddr_in server_addr;
    memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(port);
    server_addr.sin_addr.s_addr = INADDR_ANY;

    if (bind(server_fd, (struct sockaddr *)&server_addr, sizeof(server_addr)) == -1) {
        perror("bind");
        terminate(EXIT_FAILURE);
    }

    if (listen(server_fd, SOMAXCONN) == -1) {
        perror("listen");
        terminate(EXIT_FAILURE);
    }

    debug("Server listening on port %d", port);

    // accept connections and create threads
    while (!shutdown_flag) {
        struct sockaddr_in client_addr;
        socklen_t client_len = sizeof(client_addr);
        int client_fd = accept(server_fd, (struct sockaddr *)&client_addr, &client_len);

        if (client_fd == -1) {
            if (errno == EINTR) {
                if (shutdown_flag) {
                    break;
                } else {
                    continue;
                }
            }
            perror("accept");
            continue;
        }

        int *fd_ptr = malloc(sizeof(int));
        if (fd_ptr == NULL) {
            perror("malloc");
            close(client_fd);
            continue;
        }
        *fd_ptr = client_fd;

        pthread_t tid;
        if (pthread_create(&tid, NULL, pbx_client_service, fd_ptr) != 0) {
            perror("pthread_create");
            close(client_fd);
            free(fd_ptr);
            continue;
        }
        // the thread will detach itself after retrieving the fd
    }

    terminate(EXIT_SUCCESS);
}


/*
 * Signal handler for SIGHUP to terminate the server.
 */
static void terminate_handler(int signum) {
    shutdown_flag = 1;
}



/*
 * Function called to cleanly shut down the server.
 */
static void terminate(int status) {
    debug("Shutting down PBX...");
    if (server_fd >= 0) {
        close(server_fd);
        server_fd = -1;
    }
    pbx_shutdown(pbx);
    debug("PBX server terminating");
    exit(status);
}
