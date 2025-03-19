/*
 * PBX: simulates a Private Branch Exchange.
 */
#include <stdlib.h>
#include <pthread.h>
#include <unistd.h>
#include <string.h>
#include <signal.h>
#include <stdio.h>
#include <sys/socket.h>


#include "pbx.h"
#include "debug.h"

#define MAX_EXTENSIONS PBX_MAX_EXTENSIONS

struct pbx {
    pthread_mutex_t mutex;          // Mutex for synchronizing access
    TU *extensions[MAX_EXTENSIONS];  // Array of TUs indexed by extension number
    int shutdown_in_progress;       // Flag to indicate shutdown
    pthread_cond_t shutdown_cond;   // Condition variable for shutdown
    int active_tus;                 // Number of active TUs
};


/*
 * Initialize a new PBX.
 *
 * @return the newly initialized PBX, or NULL if initialization fails.
 */

PBX *pbx_init() {
    PBX *pbx = malloc(sizeof(PBX));
    if (pbx == NULL) {
        return NULL;
    }

    if (pthread_mutex_init(&pbx->mutex, NULL) != 0) {
        free(pbx);
        return NULL;
    }

    if (pthread_cond_init(&pbx->shutdown_cond, NULL) != 0) {
        pthread_mutex_destroy(&pbx->mutex);
        free(pbx);
        return NULL;
    }

    pbx->shutdown_in_progress = 0;
    pbx->active_tus = 0;

    memset(pbx->extensions, 0, sizeof(pbx->extensions));

    return pbx;
}




void pbx_shutdown(PBX *pbx) {
    pthread_mutex_lock(&pbx->mutex);

    pbx->shutdown_in_progress = 1;

    // Shutdown all network connections to registered TUs
    for (int i = 0; i < MAX_EXTENSIONS; i++) {
        TU *tu = pbx->extensions[i];
        if (tu != NULL) {
            int fd = tu_fileno(tu);
            if (fd >= 0) {
                shutdown(fd, SHUT_RD);  // Shutdown reading end to cause client service threads to notice
            }
        }
    }

    // Wait for all TUs to be unregistered
    while (pbx->active_tus > 0) {
        pthread_cond_wait(&pbx->shutdown_cond, &pbx->mutex);
    }

    pthread_mutex_unlock(&pbx->mutex);

    // Destroy mutex and condition variable
    pthread_mutex_destroy(&pbx->mutex);
    pthread_cond_destroy(&pbx->shutdown_cond);

    free(pbx);
}





int pbx_register(PBX *pbx, TU *tu, int ext) {
    if (pbx == NULL || tu == NULL || ext < 0 || ext >= MAX_EXTENSIONS) {
        return -1;
    }

    pthread_mutex_lock(&pbx->mutex);

    if (pbx->shutdown_in_progress) {
        pthread_mutex_unlock(&pbx->mutex);
        return -1;
    }

    if (pbx->extensions[ext] != NULL) {
        pthread_mutex_unlock(&pbx->mutex);
        return -1;  // Extension already in use
    }

    pbx->extensions[ext] = tu;
    pbx->active_tus++;

    tu_ref(tu, "Registering TU with PBX");
    tu_set_extension(tu, ext);

    pthread_mutex_unlock(&pbx->mutex);

    return 0;
}





int pbx_unregister(PBX *pbx, TU *tu) {
    if (pbx == NULL || tu == NULL) {
        return -1;
    }

    pthread_mutex_lock(&pbx->mutex);

    int ext = tu_extension(tu);

    if (ext < 0 || ext >= MAX_EXTENSIONS || pbx->extensions[ext] != tu) {
        pthread_mutex_unlock(&pbx->mutex);
        return -1;  // TU is not registered at this extension
    }

    pbx->extensions[ext] = NULL;
    pbx->active_tus--;

    // Hang up the TU to cancel any call in progress
    tu_hangup(tu);

    // Release the reference held by the PBX
    tu_unref(tu, "Unregistering TU from PBX");

    // Signal shutdown condition variable if shutdown is in progress
    if (pbx->shutdown_in_progress && pbx->active_tus == 0) {
        pthread_cond_signal(&pbx->shutdown_cond);
    }

    pthread_mutex_unlock(&pbx->mutex);

    return 0;
}




int pbx_dial(PBX *pbx, TU *tu, int ext) {
    if (pbx == NULL || tu == NULL) {
        return -1;
    }

    pthread_mutex_lock(&pbx->mutex);

    // Get the target TU
    TU *target_tu = NULL;

    if (ext >= 0 && ext < MAX_EXTENSIONS) {
        target_tu = pbx->extensions[ext];
    }

    pthread_mutex_unlock(&pbx->mutex);

    // Call tu_dial(), which handles the rest
    if (tu_dial(tu, target_tu) == -1) {
        return -1;
    }

    return 0;
}

