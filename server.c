#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>
#include "./comm.h"
#include "./db.h"

/*

STRUCT DEFINITIONS

*/
/*
 * Use the variables in this struct to synchronize your main thread with client
 * threads. Note that all client threads must have terminated before you clean
 * up the database.
 */
typedef struct server_control {
    pthread_mutex_t server_mutex;  // for the condition variable and num threads
    pthread_cond_t
        server_cond;  // use for waiting. wait for all clients to be canceled
    int num_client_threads;
} server_control_t;

/*
 * Controls when the clients in the client thread list should be stopped and
 * let go.
 */
typedef struct client_control {
    pthread_mutex_t go_mutex;
    pthread_cond_t go;
    int stopped;
} client_control_t;

/*
 * The encapsulation of a client thread, i.e., the thread that handles
 * commands from clients.
 */
typedef struct client {
    pthread_t thread;
    FILE *cxstr;  // File stream for input and output

    // For client list
    struct client *prev;
    struct client *next;
} client_t;

/*

 * The encapsulation of a thread that handles signals sent to the server.
 * When SIGINT is sent to the server all client threads should be destroyed.
 */
typedef struct sig_handler {
    sigset_t set;
    pthread_t thread;
} sig_handler_t;

/*

GLOBAL VARIABLES

*/
server_control_t server_controller = {PTHREAD_MUTEX_INITIALIZER,
                                      PTHREAD_COND_INITIALIZER, 0};
client_t *thread_list_head;
pthread_mutex_t thread_list_mutex = PTHREAD_MUTEX_INITIALIZER;
int accepting = 1;
pthread_mutex_t accepting_mutex = PTHREAD_MUTEX_INITIALIZER;
pthread_t sigint_thread;
client_control_t c_control = {PTHREAD_MUTEX_INITIALIZER,
                              PTHREAD_COND_INITIALIZER, 0};

/*

FORWARD DECLARATIONS

*/
void *run_client(void *arg);
void *monitor_signal(void *arg);
void thread_cleanup(void *arg);

/*

FUNCTION DEFINITIONS

*/

// Called by client threads to wait until progress is permitted
void client_control_wait() {
    // TODO: Block the calling thread until the main thread calls
    // client_control_release(). See the client_control_t struct.
    pthread_mutex_lock(&c_control.go_mutex);
    pthread_cleanup_push((void (*)(void *))pthread_mutex_unlock,
                         &c_control.go_mutex);
    while ((c_control.stopped)) {
        pthread_cond_wait(&c_control.go, &c_control.go_mutex);
    }
    pthread_cleanup_pop(1);
}

// Called by main thread to stop client threads
void client_control_stop() {
    // TODO: Ensure that the next time client threads call client_control_wait()
    // at the top of the event loop in run_client, they will block.
    pthread_mutex_lock(&c_control.go_mutex);
    c_control.stopped = 1;
    pthread_mutex_unlock(&c_control.go_mutex);
}

// Called by main thread to resume client threads
void client_control_release() {
    // TODO: Allow clients that are blocked within client_control_wait()
    // to continue. See the client_control_t struct.
    int err;

    pthread_mutex_lock(&c_control.go_mutex);
    c_control.stopped = 0;
    if ((err = pthread_cond_broadcast(&c_control.go))) {
        handle_error_en(err, "pthread_cond_broadcast, client_control_release");
    }
    pthread_mutex_unlock(&c_control.go_mutex);
}

// Called by listener (in comm.c) to create a new client thread
void client_constructor(FILE *cxstr) {
    // You should create a new client_t struct here and initialize ALL
    // of its fields. Remember that these initializations should be
    // error-checked.
    //
    // TODO:
    // Step 1: Allocate memory for a new client and set its connection stream
    // to the input argument.
    // Step 2: Create the new client thread running the run_client routine.
    // Step 3: Detach the new client thread

    // step 1
    client_t *c;
    if ((c = (client_t *)malloc(sizeof(client_t))) == 0) {
        perror("malloc client_constructor");
        fprintf(stderr, "malloc client_constructor");
    }
    c->cxstr = cxstr;
    c->prev = NULL;
    c->next = NULL;

    // step 2
    int err;
    if ((err = pthread_create(&c->thread, 0, run_client, c))) {
        handle_error_en(err, "pthread_create client_constructor");
    }

    // step 3
    if ((err = pthread_detach(c->thread))) {
        handle_error_en(err, "pthread_detach");
    }
}

void client_destructor(client_t *client) {
    // TODO: Free and close all resources associated with a client.
    // Whatever was malloc'd in client_constructor should
    // be freed here!
    comm_shutdown(client->cxstr);
    free(client);
}

void add_client_to_list(client_t *c) {
    // add client
    pthread_mutex_lock(&thread_list_mutex);  // lock linked list
    if (thread_list_head != NULL) {
        thread_list_head->prev = c;
        c->next = thread_list_head;
        thread_list_head = c;
    } else {
        thread_list_head = c;
    }

    // increment server_control counter
    pthread_mutex_lock(&server_controller.server_mutex);  // lock server mutex
    server_controller.num_client_threads += 1;
    pthread_mutex_unlock(
        &server_controller.server_mutex);  // unlock server mutex

    pthread_mutex_unlock(&thread_list_mutex);  // unlock linked list
}

// Code executed by a client thread
void *run_client(void *arg) {
    // TODO:
    // Step 1: Make sure that the server is still accepting clients. This will
    //         will make sense when handling EOF for the server.
    // Step 2: Add client to the client list and push thread_cleanup to remove
    //       it if the thread is canceled.
    // Step 3: Loop comm_serve (in comm.c) to receive commands and output
    //       responses. Execute commands using interpret_command (in db.c)
    // Step 4: When the client is done sending commands, exit the thread
    //       cleanly.
    //
    // You will need to modify this when implementing functionality for stop and
    // go!

    client_t *c = (client_t *)arg;

    // step 1
    if (accepting == 1) {
        // step 2
        add_client_to_list(c);
        pthread_cleanup_push(thread_cleanup, c);

        // create command and response buffers for client
        char command[BUFLEN];
        char response[BUFLEN];
        memset(command, 0, BUFLEN);
        memset(response, 0, BUFLEN);

        // wait for client response
        while (comm_serve(c->cxstr, response, command) == 0) {
            client_control_wait();
            interpret_command(command, response, BUFLEN);
        }
        pthread_cleanup_pop(1);
    } else {
        client_destructor(c);
    }

    return NULL;
}

void delete_all() {
    // TODO: Cancel every thread in the client thread list with the
    // pthread_cancel function.
    pthread_mutex_lock(&thread_list_mutex);
    client_t *curr = thread_list_head;
    while (curr != NULL) {
        pthread_cancel(curr->thread);
        curr = curr->next;

        // decrement client counter
        pthread_mutex_lock(&server_controller.server_mutex);
        server_controller.num_client_threads -= 1;
        pthread_mutex_unlock(&server_controller.server_mutex);
    }

    pthread_mutex_unlock(&thread_list_mutex);
}

// Cleanup routine for client threads, called on cancels and exit.
void thread_cleanup(void *arg) {
    // TODO: Remove the client object from thread list and call
    // client_destructor. This function must be thread safe! The client must
    // be in the list before this routine is ever run.

    client_t *c = (client_t *)arg;

    // remove client from thread_list
    pthread_mutex_lock(&thread_list_mutex);
    if (c == thread_list_head) {  // case where c is the head
        if (c->next == NULL) {
            thread_list_head = NULL;
        } else {
            c->prev = NULL;
            thread_list_head = c->next;
        }
    } else if (c->next == NULL) {
        c->prev->next = NULL;
    } else {
        c->prev->next = c->next;
        c->next->prev = c->prev;
    }
    pthread_mutex_unlock(&thread_list_mutex);

    // decrement client counter
    pthread_mutex_lock(&server_controller.server_mutex);
    server_controller.num_client_threads -= 1;
    pthread_mutex_unlock(&server_controller.server_mutex);

    // deconstruct client
    client_destructor(c);
}

// Code executed by the signal handler thread. For the purpose of this
// assignment, there are two reasonable ways to implement this.
// The one you choose will depend on logic in sig_handler_constructor.
// 'man 7 signal' and 'man sigwait' are both helpful for making this
// decision. One way or another, all of the server's client threads
// should terminate on SIGINT. The server (this includes the listener
// thread) should not, however, terminate on SIGINT!
void *monitor_signal(void *arg) {
    // TODO: Wait for a SIGINT to be sent to the server process and cancel
    // all client threads when one arrives.
    sigset_t *set = (sigset_t *)arg;
    int signal;
    sigwait(set, &signal);
    if (signal != SIGINT) {
        perror("sigwait error");
        fprintf(stderr, "sigwait in monitor_signal");
    }
    delete_all();
    return NULL;
}

sig_handler_t *sig_handler_constructor() {
    // TODO: Create a thread to handle SIGINT. The thread that this function
    // creates should be the ONLY thread that ever responds to SIGINT.

    int err;
    // malloc handler and error check
    sig_handler_t *handler;
    if (((handler = (sig_handler_t *)malloc(sizeof(sig_handler_t))) == 0)) {
        perror("malloc sig_handler_constructor");
        fprintf(stderr, "sig_handler_constructor");
    }

    // empty then fill set with signals it needs to handle (only SIGINT)
    sigemptyset(&handler->set);
    sigaddset(&handler->set, SIGINT);

    // mask off sigints
    if ((err = pthread_sigmask(SIG_BLOCK, &handler->set, 0))) {
        handle_error_en(err, "pthread_sigmask SIGINT, sig_handler_constructor");
    }

    // create thread for sig_handler_t, passing in monitor_signal as its routine
    if ((err = pthread_create(&handler->thread, 0, monitor_signal,
                              &handler->set))) {
        handle_error_en(err, "pthread_create, sig_handler_constructor");
    }

    return handler;
}

void sig_handler_destructor(sig_handler_t *sighandler) {
    // TODO: Free any resources allocated in sig_handler_constructor.
    // Cancel and join with the signal handler's thread.
    int err;
    if ((err = pthread_cancel(sighandler->thread))) {
        handle_error_en(err, "pthread_cancel, sig_handler_destructor");
    }
    if ((err = pthread_join(sighandler->thread, NULL))) {
        handle_error_en(err, "pthread_join, sig_handler_destructor");
    }
    free(sighandler);
}

void parse(char buf[BUFLEN]) {
    char *delimiter = " \t\n";
    char *tok = strtok(buf, delimiter);

    if ((strcmp(tok, "p")) == 0) {
        if ((tok = strtok(NULL, delimiter)) != NULL) {
            db_print(tok);
            fprintf(stderr, "database printed into file\n");
        } else {
            db_print(0);
        }
    } else if ((strcmp(tok, "s")) == 0) {
        client_control_stop();
        fprintf(stderr, "all clients paused\n");
    } else if ((strcmp(tok, "g")) == 0) {
        client_control_release();
        fprintf(stderr, "clients released\n");
    } else {
        fprintf(stderr, "invlaid server command\n");
    }

    return;
}

// The arguments to the server should be the port number.
int main(int argc, char *argv[]) {
    // TODO:

    // Step 1: Set up the signal handler for handling SIGINT.
    // Step 2: block SIGPIPE so that the server does not abort when a client
    // disocnnects Step 3: Start a listener thread for clients (see
    // start_listener in
    //       comm.c).
    // Step 4: Loop for command line input and handle accordingly until EOF.
    // Step 5: Destroy the signal handler, delete all clients, cleanup the
    //       database, cancel and join with the listener thread
    //
    // You should ensure that the thread list is empty before cleaning up the
    // database and canceling the listener thread. Think carefully about what
    // happens in a call to delete_all() and ensure that there is no way for a
    // thread to add itself to the thread list after the server's final
    // delete_all().

    // if eof or sigint not working, see how you edit the linked list
    int err;

    // step 1- block SIGPIPE
    sigset_t set;
    sigemptyset(&set);
    sigaddset(&set, SIGPIPE);
    if ((err = pthread_sigmask(SIG_BLOCK, &set, 0))) {
        handle_error_en(err, "pthread_sigmask SIGPIPE, main");
    }

    // step 2- set up SIGINT handler
    sig_handler_t *sighandler = sig_handler_constructor();

    // step 3- start listening thread
    int port = atoi(argv[1]);
    pthread_t listener = start_listener(port, client_constructor);

    // step 4
    while (1) {
        char buf[BUFLEN];  // buffer contains input from server STDIN
        memset(buf, 0, BUFLEN);
        ssize_t input = read(0, buf, BUFLEN);

        if (input < 0) {  // -1 return from read: there was an error
            perror("read main.c");
            exit(1);
        } else if (input == 0) {  // EOF case
            // clean up signal handler thread
            sig_handler_destructor(sighandler);

            // stop accepting inout from clients
            pthread_mutex_lock(&accepting_mutex);
            accepting = 0;
            pthread_mutex_unlock(&accepting_mutex);

            // cancel all client threads
            delete_all();

            /* need to wait for all client threads to be canceled first
            before cleaning up database and listener thread */
            pthread_mutex_lock(&server_controller.server_mutex);
            while ((server_controller.num_client_threads) != 0) {
                pthread_cond_wait(&server_controller.server_cond,
                                  &server_controller.server_mutex);
            }
            pthread_mutex_unlock(&server_controller.server_mutex);

            // clean up database and listener thread
            db_cleanup();
            if ((err = pthread_cancel(listener))) {
                handle_error_en(err, "pthread_cancel listener, eof");
            }
            if ((err = pthread_join(listener, 0))) {
                handle_error_en(err, "pthread_join listener, eof");
            }
            pthread_exit((void *)0);

        } else if (input == 1) {  // case of a blank line (input == 1)
            continue;
        } else {                // there was input
            buf[input] = '\0';  // terminate the buffer to avoid junk values
            parse(buf);
        }
    }

    return 0;
}
