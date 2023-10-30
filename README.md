# Database
Overall structure of your code:

server.c
- main function masks off SIGPIPE and sets up a signal handler thread for SIGINT. It also
starts the listener thread and the server REPL, which handles server commands and
EOF for the server. When EOF happens, all client threads are canceled, then the signal
handler thread, then the listener thread, and finally, the main thread is exited.
- when clients are connected, a new client struct is created and given a thread. 
thread_cleanup is pushed onto each of these threads to properly clean
up resourses when it's popped. the thread is then detached. when a client is created,
its thread is added to the doubly linked list
- there is a dedicated sig_handler_t which has a thread and a sigset. the only signal
added to the set is SIGINT, and the thread's routine is monitor_signal, which uses sigwait
to wait for the main thread to recieve a SIGINT in which case it will delete_all() client
threads
- the server_control struct has a mutex, condition variable, and counter. the counter is
used to count the number of clients on the server. We wait on this counter later when handling
EOF to make sure that all clients are terminated (num_client_threads == 0) before cleaning
up the data base and canceling the rest of the threads. the mutex is to make sure the counter
and condition are thread safe. 
- the client control struct also has a mutex and condition variable, as well as a "stopped"
flag. this flag is used to pause service to clients, if someone types "s" into the server
terminal. Using a condition variables and mutex in a similar fashion to the server_controller,
clients wait on the flag to become 0 (client_control_release is called), at which point 
run_client (the routine for each client) can finally continue and run interpret command.
- I created an "accepting" flag and a mutex for that flag which allows clients to connect to
the server. If it is 0, which only happens in EOF, then no more client connections can be made
to the server

db.c
- the vast majority of db.c was written for us, I just made the functions, search, add, remove,
print_recurs, and query thread safe using rwlocks, to implement fine grained locking on the db.
- the idea is to use "hand over hand locking" which means locking your child before releasing the
parent to prevent possible deadlocks.

Helper functions and what they do:

In server.c
1) void add_client_to_list(client_t *c) adds a client to the doubly linked list of
client threads. Is thread safe, called within run_client. takes in the client to be
added.
2) void parse(char buf[BUFLEN]) parses input for the server REPL. takes in the buffer
containing the user input (STDIN from server). handles stop, go, and print
commands.

In db.c
1) void lock(locktype lt, pthread_rwlock_t *lk) performs a lock on the passed in
rwlock. Whether it performs a write or read lock depends on the locktype enum passed
in.

In db.h
1) typedef enum { l_read, l_write } locktype is an enum that describes whether a rwlock
should be a read or write lock. Used in db.c

Changes to any function signatures: none.

Unresolved bugs: eof seg faults/ does not exit properly in very few cases.
