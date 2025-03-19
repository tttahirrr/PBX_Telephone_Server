
#include <stdlib.h>
#include <pthread.h>
#include <unistd.h>
#include <stdio.h>
#include <errno.h>
#include <string.h>

#include "pbx.h"
#include "debug.h"
#include "tu.h"


struct tu {
    int ext;
    int fd;
    int refs;
    pthread_mutex_t mutex;
    struct tu *peer;
    TU_STATE state;
};

TU *tu_init(int fd) {
    if (fd < 0) {
        // invalid file descriptor
        return NULL;
    }

    // allocate memory for the TU
    TU *tu = malloc(sizeof(struct tu));
    if (tu == NULL) {
        // mem allocation failed
        return NULL;
    }

    // initialize the mutex
    if (pthread_mutex_init(&tu->mutex, NULL) != 0) {
        // Mutex initialization failed
        free(tu);
        return NULL;
    }

    // initialize TU fields
    tu->refs = 1;            // initial reference count
    tu->fd = fd;             // store the file descriptor
    tu->ext = -1;            // extension number to be set later
    tu->state = TU_ON_HOOK;  // initial state
    tu->peer = NULL;         // no peer initially


    return tu;
}

void tu_ref(TU *tu, char *reason) {
    if(tu == NULL)
        return;
    tu->refs++;
}
void tu_unref(TU *tu, char *reason) {
    if(tu == NULL)
        return;
    tu->refs--;
    if(tu->refs == 0){
        close(tu->fd);
        pthread_mutex_destroy(&tu->mutex);
        free(tu);
    } 
}
int tu_fileno(TU *tu) {
    if(tu == NULL)
        return -1;
    pthread_mutex_lock(&tu->mutex);
    int tu_fd = tu->fd;
    pthread_mutex_unlock(&tu->mutex);
    return tu_fd;
}
int tu_extension(TU *tu) {
    if(tu == NULL){
        return -1;
    }
    int tu_ext = tu->ext;
    return tu_ext;
}
int tu_set_extension(TU *tu, int ext) {
    if(tu == NULL || ext > PBX_MAX_EXTENSIONS || ext < 0){
        return -1;
    }
    pthread_mutex_lock(&tu->mutex);
    tu->ext = ext;
    char temp[256];
    snprintf(temp, sizeof(temp), "ON HOOK %d%s", tu->ext, EOL);
    write(tu->fd, temp, strlen(temp));
    pthread_mutex_unlock(&tu->mutex);
    return 0;
}



static int notify_state(TU *x) {
    if (!x) {
        debug("notify_state: TU pointer is NULL, cannot notify.");
        return -1;
    }

    debug("notify_state: Notifying TU at extension %d of its state.", x->ext);

    const char *msg = NULL;
    char temp[256];

    // determine msg based on state
    if (x->state == TU_ON_HOOK) {
        snprintf(temp, sizeof(temp), "ON HOOK %d\r\n", x->ext);
        msg = temp;
    } else if (x->state == TU_RINGING) {
        msg = "RINGING\r\n";
    } else if (x->state == TU_DIAL_TONE) {
        msg = "DIAL TONE\r\n";
    } else if (x->state == TU_RING_BACK) {
        msg = "RING BACK\r\n";
    } else if (x->state == TU_BUSY_SIGNAL) {
        msg = "BUSY SIGNAL\r\n";
    } else if (x->state == TU_CONNECTED) {
        int e;
        if (x->peer)
            e = x->peer->ext;
        else
            e = -1;
        snprintf(temp, sizeof(temp), "CONNECTED %d\r\n", e);
        msg = temp;
    } else if (x->state == TU_ERROR) {
        msg = "ERROR\r\n";
    } else {
        msg = "UNKNOWN STATE\r\n";
    }

    if (!msg) {
        debug("notify_state: No message determined for TU at extension %d.", x->ext);
        return -1;
    }

    // this is as robust as possible
    size_t remaining = strlen(msg);
    const char *ptr = msg;
    while (remaining > 0) {
        ssize_t written = write(x->fd, ptr, remaining);
        if (written < 0) {
            if (errno == EINTR) {
                debug("notify_state: Write interrupted by signal, retrying.");
                continue;
            }
            debug("notify_state: Error writing to FD %d for TU at ext %d (errno=%d).", x->fd, x->ext, errno);
            return -1;
        }
        if (written == 0) {
            debug("notify_state: Unexpected EOF on write to FD %d for TU at ext %d.", x->fd, x->ext);
            return -1;
        }
        remaining -= written;
        ptr += written;
    }

    debug("notify_state: Successfully notified TU at extension %d.", x->ext);
    return 0;
}

int tu_dial(TU *tu, TU *target) {
    debug("tu_dial: Entered function.");

    // check input parameters
    if (!tu) {
        debug("tu_dial: TU pointer is NULL.");
        return -1;
    }

    if (target == NULL) {
        // target is NULL and TU in DIAL_TONE
        debug("tu_dial: target is NULL and TU is DIAL_TONE, transitioning to ERROR.");
        pthread_mutex_lock(&tu->mutex);
        tu->state = TU_ERROR;
        int ret = notify_state(tu);
        pthread_mutex_unlock(&tu->mutex);
        if (ret < 0) {
            debug("tu_dial: Failed to notify TU after transitioning to ERROR.");
        }
        return -1;
    }

    // target is not NULL
    debug("tu_dial: Target is not NULL, proceeding with call attempt.");

    // make sure target is a valid TU
    if (!target) {
        debug("tu_dial: Target pointer became NULL unexpectedly.");
        return -1;
    }

    // determine lock order to avoid deadlock
    TU *first_lock;
    TU *second_lock;

    if (tu < target) {
        first_lock = tu;
        second_lock = target;
    } else {
        first_lock = target;
        second_lock = tu;
    }

    pthread_mutex_lock(&first_lock->mutex);
    if (first_lock != second_lock) {
        pthread_mutex_lock(&second_lock->mutex);
    }


    // check target state
    if (target->peer != NULL || target->state != TU_ON_HOOK) {
        debug("tu_dial: Target TU is not ON_HOOK or has a peer, switching caller to BUSY_SIGNAL.");
        tu->state = TU_BUSY_SIGNAL;
        int ret = notify_state(tu);
        if (ret < 0) {
            debug("tu_dial: Failed to notify TU after BUSY_SIGNAL set.");
        }
        if (first_lock != second_lock) pthread_mutex_unlock(&second_lock->mutex);
        pthread_mutex_unlock(&first_lock->mutex);
        return 0;
    }

    // handle dialing self
    if (tu == target) {
        debug("tu_dial: TU attempting to dial itself, switching to BUSY_SIGNAL.");
        tu->state = TU_BUSY_SIGNAL;
        int ret = notify_state(tu);
        if (ret < 0) {
            debug("tu_dial: Failed to notify TU after dialing itself.");
        }
        if (first_lock != second_lock) pthread_mutex_unlock(&second_lock->mutex);
        pthread_mutex_unlock(&first_lock->mutex);
        return 0;
    }

    

    // both TUs are free and target is ON_HOOK
    debug("tu_dial: Establishing connection. Caller -> RING_BACK, Callee -> RINGING.");
    tu->peer = target;
    target->peer = tu;
    tu_ref(tu, "tu_dial: originator gains peer");
    tu_ref(target, "tu_dial: target gains peer");

    tu->state = TU_RING_BACK;
    target->state = TU_RINGING;

    int ret_calling = notify_state(tu);
    if (ret_calling < 0) {
        debug("tu_dial: Error notifying calling TU of RING_BACK state.");
    }

    int ret_target = notify_state(target);
    if (ret_target < 0) {
        debug("tu_dial: Error notifying target TU of RINGING state.");
    }

    if (first_lock != second_lock) pthread_mutex_unlock(&second_lock->mutex);
    pthread_mutex_unlock(&first_lock->mutex);

    debug("tu_dial: Call setup completed successfully.");
    return 0;
}


int tu_pickup(TU *tu) {
    debug("tu_pickup: Entered function.");

    if (tu == NULL) {
        debug("tu_pickup: TU pointer is NULL!");
        return -1;
    }

    pthread_mutex_lock(&tu->mutex);
    TU_STATE state = tu->state;
    TU *peer = tu->peer;
    debug("tu_pickup: TU ext=%d initial state=%d peer=%p", tu->ext, state, (void*)peer);

    // handle states
    switch (state) {
        case TU_ON_HOOK:
            // transition to DIAL_TONE
            tu->state = TU_DIAL_TONE;
            debug("tu_pickup: Transitioning TU ext=%d from ON_HOOK to DIAL_TONE.", tu->ext);
            if (notify_state(tu) < 0) {
                debug("tu_pickup: Failed to notify TU ext=%d of DIAL_TONE state.", tu->ext);
            }
            pthread_mutex_unlock(&tu->mutex);
            return 0;

        case TU_RINGING:
            // unlock tu before locking both TU and peer
            pthread_mutex_unlock(&tu->mutex);

            // determine lock order to avoid deadlock
            TU *first_lock;
            TU *second_lock;

            if (tu < peer) {
                first_lock = tu;
                second_lock = peer;
            } else {
                first_lock = peer;
                second_lock = tu;
            }

            pthread_mutex_lock(&first_lock->mutex);
            if (first_lock != second_lock)
                pthread_mutex_lock(&second_lock->mutex);


            // both TUs are in the expected RINGING/RING_BACK states
            debug("tu_pickup: Transitioning TU ext=%d and Peer ext=%d to CONNECTED state.", tu->ext, peer->ext);
            tu->state = TU_CONNECTED;
            peer->state = TU_CONNECTED;

            // notify both TUs
            if (notify_state(tu) < 0) {
                debug("tu_pickup: Failed to notify TU ext=%d of CONNECTED state.", tu->ext);
            }
            if (notify_state(peer) < 0) {
                debug("tu_pickup: Failed to notify Peer ext=%d of CONNECTED state.", peer->ext);
            }

            if (first_lock != second_lock)
                pthread_mutex_unlock(&second_lock->mutex);
            pthread_mutex_unlock(&first_lock->mutex);

            return 0;

        default:
            // all other states, just notify current state, no state change
            debug("tu_pickup: TU ext=%d is in state=%d, no state change on pickup.", tu->ext, state);
            if (notify_state(tu) < 0)
                debug("tu_pickup: Failed to notify TU ext=%d in default state case.", tu->ext);
            pthread_mutex_unlock(&tu->mutex);
            return 0;
    }
}


int tu_hangup(TU *tu) {
    debug("tu_hangup: Function start.");

    if (tu == NULL) {
        debug("tu_hangup: TU is NULL, cannot proceed.");
        return -1;
    }

    TU_STATE state = tu->state;
    TU *conn_peer = tu->peer;
    debug("tu_hangup: TU ext=%d initial state=%d, peer=%p", tu->ext, state, (void*)conn_peer);

    switch (state) {
        case TU_CONNECTED:
        case TU_RINGING:
        case TU_RING_BACK: {

            debug("tu_hangup: Handling hangup with peer ext=%d in state=%d.", conn_peer->ext, state);

            TU *first_lock;
            TU *second_lock;
            if (tu < conn_peer) {
                first_lock = tu;
                second_lock = conn_peer;
            } else {
                first_lock = conn_peer;
                second_lock = tu;
            }

            pthread_mutex_lock(&first_lock->mutex);
            if (first_lock != second_lock) {
                pthread_mutex_lock(&second_lock->mutex);
            }

            TU_STATE recheck_state = tu->state;
            debug("tu_hangup: After locking, TU ext=%d state=%d, peer ext=%d state=%d",
                  tu->ext, recheck_state, conn_peer->ext, conn_peer->state);

            if (recheck_state == TU_RING_BACK) {
                debug("tu_hangup: TU ext=%d and peer ext=%d: both going ON_HOOK.", tu->ext, conn_peer->ext);

                tu->state = TU_ON_HOOK;
                tu->peer = NULL;
                conn_peer->state = TU_ON_HOOK;
                conn_peer->peer = NULL;

                tu_unref(conn_peer, "hangup ringback disconnection (peer)");
                tu_unref(tu, "hangup ringback disconnection (self)");

                int ret_tu_notify = notify_state(tu);
                if (ret_tu_notify < 0) {
                    debug("tu_hangup: notify_state failed for TU ext=%d after ON_HOOK (ringback).", tu->ext);
                }

                int ret_peer_notify = notify_state(conn_peer);
                if (ret_peer_notify < 0) {
                    debug("tu_hangup: notify_state failed for peer ext=%d after ON_HOOK (ringback).", conn_peer->ext);
                }

                if (first_lock != second_lock) pthread_mutex_unlock(&second_lock->mutex);
                pthread_mutex_unlock(&first_lock->mutex);
                return 0;
            } else if (recheck_state == TU_RINGING || recheck_state == TU_CONNECTED) {
                debug("tu_hangup: TU ext=%d and peer ext=%d: transitioning to ON_HOOK and DIAL_TONE.", tu->ext, conn_peer->ext);
                
                tu->state = TU_ON_HOOK;
                tu->peer = NULL;
                conn_peer->state = TU_DIAL_TONE;
                conn_peer->peer = NULL;

                tu_unref(conn_peer, "hangup disconnection (peer)");
                tu_unref(tu, "hangup disconnection (self)");

                int ret_tu_notify = notify_state(tu);
                if (ret_tu_notify < 0) {
                    debug("tu_hangup: notify_state failed for TU ext=%d after ON_HOOK set.", tu->ext);
                }

                int ret_peer_notify = notify_state(conn_peer);
                if (ret_peer_notify < 0) {
                    debug("tu_hangup: notify_state failed for peer ext=%d after DIAL_TONE set.", conn_peer->ext);
                }

                if (first_lock != second_lock) pthread_mutex_unlock(&second_lock->mutex);
                pthread_mutex_unlock(&first_lock->mutex);
                return 0;
            }

            debug("tu_hangup: None of the expected states matched after re-check for TU ext=%d.", tu->ext);
            {
                int ret_notify_fallback = notify_state(tu);
                if (ret_notify_fallback < 0) {
                    debug("tu_hangup: notify_state failed in fallback scenario for TU ext=%d.", tu->ext);
                }
            }

            if (first_lock != second_lock) pthread_mutex_unlock(&second_lock->mutex);
            pthread_mutex_unlock(&first_lock->mutex);
            return -1;
        }

        case TU_DIAL_TONE:
        case TU_BUSY_SIGNAL:
        case TU_ERROR: {
            debug("tu_hangup: TU ext=%d in simple state=%d, transitioning to ON_HOOK.", tu->ext, state);
            pthread_mutex_lock(&tu->mutex);
            tu->state = TU_ON_HOOK;
            int ret_notify = notify_state(tu);
            if (ret_notify < 0) {
                debug("tu_hangup: notify_state failed after ON_HOOK set in simple states for ext=%d.", tu->ext);
            }
            pthread_mutex_unlock(&tu->mutex);
            return 0;
        }

        default: {
            debug("tu_hangup: TU ext=%d in unhandled state=%d, just notify current state.", tu->ext, state);
            pthread_mutex_lock(&tu->mutex);
            int ret_notify = notify_state(tu);
            if (ret_notify < 0) {
                debug("tu_hangup: notify_state failed in default state scenario for TU ext=%d.", tu->ext);
            }
            pthread_mutex_unlock(&tu->mutex);
            return 0;
        }
    }
}








int tu_chat(TU *tu, char *msg) {
    if (tu == NULL) {
        debug("tu_chat: TU is NULL.");
        return -1;
    }

    // first lock TU to safely check its state and peer
    pthread_mutex_lock(&tu->mutex);
    TU *conn_peer = tu->peer;


    // TU is connected. we need to lock the peer as well for safe I/O
    TU *first_lock;
    TU *second_lock;

    if (tu < conn_peer) {
        first_lock = tu;
        second_lock = conn_peer;
    } else {
        first_lock = conn_peer;
        second_lock = tu;
    }



    if (first_lock == tu) {
        if (second_lock != first_lock) {
            pthread_mutex_lock(&second_lock->mutex);
        }
    } else {
        // first_lock must be peer
        pthread_mutex_unlock(&tu->mutex);
        pthread_mutex_lock(&first_lock->mutex);
        if (first_lock != second_lock) {
            pthread_mutex_lock(&second_lock->mutex);
        }
    }

    // now we hold both locks. tu and peer
    char temp[1024];
    snprintf(temp, sizeof(temp), "CHAT %s\r\n", msg);

    size_t remaining = strlen(temp);
    const char *ptr = temp;
    while (remaining > 0) {
        ssize_t written = write(conn_peer->fd, ptr, remaining);
        if (written < 0) {
            if (errno == EINTR) {
                continue;
            }
            debug("tu_chat: Error writing to peer ext=%d fd=%d (errno=%d).", conn_peer->ext, conn_peer->fd, errno);
            // notify TU of its own state before returning
            if (notify_state(tu) < 0) {
                debug("tu_chat: Failed to notify TU ext=%d after chat write error.", tu->ext);
            }

            if (first_lock != second_lock) pthread_mutex_unlock(&second_lock->mutex);
            pthread_mutex_unlock(&first_lock->mutex);
            return -1;
        }
        if (written == 0) {
            debug("tu_chat: Unexpected EOF on write to peer ext=%d fd=%d.", conn_peer->ext, conn_peer->fd);
            // notify TU of its own state before returning
            if (notify_state(tu) < 0) {
                debug("tu_chat: Failed to notify TU ext=%d after EOF on write.", tu->ext);
            }

            if (first_lock != second_lock) pthread_mutex_unlock(&second_lock->mutex);
            pthread_mutex_unlock(&first_lock->mutex);
            return -1;
        }
        remaining -= written;
        ptr += written;
    }

    // chat message successfully sent to the peer.
    // notify the calling TU of its state
    if (notify_state(tu) < 0) {
        debug("tu_chat: Failed to notify TU ext=%d after successful chat.", tu->ext);
    }

    if (first_lock != second_lock) pthread_mutex_unlock(&second_lock->mutex);
    pthread_mutex_unlock(&first_lock->mutex);

    return 0;
}



