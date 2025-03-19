
/*
 * "PBX" server module.
 * Manages interaction with a client telephone unit (TU).
 */
#include <stdlib.h>
#include <unistd.h>
#include <stdio.h>
#include <string.h>
#include <pthread.h>
#include <errno.h>
#include <ctype.h>


#include "debug.h"
#include "pbx.h"
#include "server.h"





void *pbx_client_service(void *arg) {
    // Retrieve the file descriptor from arg
    int client_fd = *(int *)arg;
    free(arg);

    // Detach the thread
    pthread_detach(pthread_self());

    // Initialize a new TU with the client file descriptor
    TU *tu = tu_init(client_fd);
    if (tu == NULL) {
        close(client_fd);
        return NULL;
    }

    // Register the TU with the PBX under an extension number
    int ext = client_fd; 
    if (pbx_register(pbx, tu, ext) == -1) {
        tu_unref(tu, "Failed to register TU");
        close(client_fd);
        return NULL;
    }

    // Open a FILE stream for buffered I/O
    FILE *client_stream = fdopen(client_fd, "r");
    if (client_stream == NULL) {
        perror("fdopen");
        pbx_unregister(pbx, tu);
        tu_unref(tu, "Closing TU due to fdopen failure");
        close(client_fd);
        return NULL;
    }

    char *line = NULL;
    size_t len = 0;

    // Service loop
    while (!feof(client_stream)) {
        ssize_t nread = getline(&line, &len, client_stream);
        if (nread == -1) {
            break;  // EOF or error
        }

        // Remove trailing "\r\n" or "\n"
        char *cmd = line;
        size_t cmd_len = nread;
        while (cmd_len > 0 && (cmd[cmd_len - 1] == '\n' || cmd[cmd_len - 1] == '\r')) {
            cmd[--cmd_len] = '\0';
        }

        // Skip leading whitespace
        while (isspace((unsigned char)*cmd)) {
            cmd++;
        }

        // Parse and handle commands
        if (strncmp(cmd, "pickup", 6) == 0 && (cmd[6] == '\0' || isspace((unsigned char)cmd[6]))) {
            debug("Received 'pickup' command from extension %d", ext);
            if (tu_pickup(tu) == -1) {
                debug("Error handling 'pickup' command for extension %d", ext);
            }
        } else if (strncmp(cmd, "hangup", 6) == 0 && (cmd[6] == '\0' || isspace((unsigned char)cmd[6]))) {
            debug("Received 'hangup' command from extension %d", ext);
            if (tu_hangup(tu) == -1) {
                debug("Error handling 'hangup' command for extension %d", ext);
            }
        } else if (strncmp(cmd, "dial", 4) == 0 && isspace((unsigned char)cmd[4])) {
            // Extract extension number
            char *ext_str = cmd + 4;
            while (isspace((unsigned char)*ext_str)) {
                ext_str++;
            }
            int dial_ext = atoi(ext_str);
            debug("Received 'dial %d' command from extension %d", dial_ext, ext);
            if (pbx_dial(pbx, tu, dial_ext) == -1) {
                debug("Error handling 'dial %d' command for extension %d", dial_ext, ext);
            }
        } else if (strncmp(cmd, "chat", 4) == 0 && (cmd[4] == '\0' || isspace((unsigned char)cmd[4]))) {
            // Extract chat message
            char *msg = cmd + 4;
            while (isspace((unsigned char)*msg)) {
                msg++;
            }
            debug("Received 'chat' command from extension %d: %s", ext, msg);
            if (tu_chat(tu, msg) == -1) {
                debug("Error handling 'chat' command for extension %d", ext);
            }
        } else {
            debug("Received invalid command from extension %d: %s", ext, cmd);
        }
    }

    // Handle client disconnection as a hangup
    debug("Client at extension %d disconnected", ext);
    tu_hangup(tu);

    // Clean up
    if (line != NULL) {
        free(line);
    }
    fclose(client_stream);  // This also closes client_fd

    // Unregister the TU and decrease its reference count
    pbx_unregister(pbx, tu);
    tu_unref(tu, "Client service thread exiting");

    return NULL;
}
