
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <signal.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <pthread.h>

#define DEFAULT_PORT 12345
#define MAX_USERNAME 32
#define MAX_MESSAGE 1024
#define MAX_CLIENTS 128

// Server password
#define SERVER_PASSWORD "PleaseGiveUsExtraCredit:)"

/**
 * @brief Client structure representing a connected client.
 * 
 */
typedef struct client {
    // socket file descriptor
    int sockfd; 

    // username of the client
    char username[MAX_USERNAME];

    // 0 = not logged in yet, 1 = logged in
    int logged_in; 

    // next client in the list
    struct client *next; 
} client_t;

/**
 * @brief Message structure representing a message in the queue.
 */
typedef struct message {
    // sender username
    char sender[MAX_USERNAME];

    // message text
    char text[MAX_MESSAGE];

    // next message in the queue
    struct message *next;
} message_t;


// Global client list (linked list)
static client_t *clients_head = NULL; // Defines the client list head
static pthread_mutex_t clients_mutex = PTHREAD_MUTEX_INITIALIZER; // Mutex for client list to protect concurrent access

// Message queue (linked list)
static message_t *msg_head = NULL; // Start of the message queue
static message_t *msg_tail = NULL; // End of the message queue
static pthread_mutex_t msg_mutex = PTHREAD_MUTEX_INITIALIZER; // Mutex for message queue
static pthread_cond_t msg_cond = PTHREAD_COND_INITIALIZER; // Condition variable for message queue that signals when new messages arrive

static int server_sock = -1; // Server socket file descriptor
static volatile int server_running = 1; // Server running flag

/**
 *  @brief Sends all bytes in the buffer to the specified file descriptor.
 * 
 * @details This function attempts to send the entire buffer of length 'len' to the
 * 
 * @param fd The file descriptor to send data to.
 * @param buf Pointer to the buffer containing data to send.
 * @param len The length of the buffer in bytes.
 * @return ssize_t : The total number of bytes sent, or -1 on error.
 * 
 */
ssize_t send_all(int fd, const void *buf, size_t len) {
    size_t total = 0;
    const char *p = buf;
    while (total < len) {
        ssize_t n = send(fd, p + total, len - total, 0);
        if (n <= 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        total += n;
    }
    return total;
}

/**
 * @brief Broadcasts a formatted message to all logged-in clients.
 * 
 * @param sender The username of the sender.
 * @param text The message text to broadcast.
 * 
 */
void broadcast_formatted(const char *sender, const char *text) {
    // format: username: text\n
    char out[MAX_USERNAME + 2 + MAX_MESSAGE + 2];
    snprintf(out, sizeof(out), "%s: %s\n", sender, text);

    pthread_mutex_lock(&clients_mutex);
    client_t *c = clients_head;

    // While the client is active, check to see if the other clients are active.
    // We can make this into a function later in the future if we want to specify a minumum number of clients
    while (c) {
        if (c->logged_in) {
            if (send_all(c->sockfd, out, strlen(out)) < 0) {
                // ignore error here; the client thread will handle closure
            }
        }
        c = c->next;
    }
    pthread_mutex_unlock(&clients_mutex);
}


/**
 * @brief Enqueues a message to the message queue.
 * 
 * @param sender The username of the sender.
 * @param text The message text.
 */
void enqueue_message(const char *sender, const char *text) {
    message_t *m = calloc(1, sizeof(message_t));
    if (!m) return; // allocation failed
    strncpy(m->sender, sender, MAX_USERNAME-1); // Send the sender username
    strncpy(m->text, text, MAX_MESSAGE-1); // Send text
    m->next = NULL;

    pthread_mutex_lock(&msg_mutex);
    if (!msg_tail) {
        msg_head = msg_tail = m;
    } else {
        msg_tail->next = m;
        msg_tail = m;
    }
    pthread_cond_signal(&msg_cond);
    pthread_mutex_unlock(&msg_mutex);
}

/**
 * @brief Dequeues a message from the message queue.
 * 
 * @return message_t* Pointer to the dequeued message, or NULL if server is shutting down.
 */
message_t *dequeue_message() {
    pthread_mutex_lock(&msg_mutex);
    while (!msg_head && server_running) {
        pthread_cond_wait(&msg_cond, &msg_mutex);
    }
    if (!server_running) {
        pthread_mutex_unlock(&msg_mutex);
        return NULL;
    }
    message_t *m = msg_head;
    msg_head = msg_head->next;
    if (!msg_head) msg_tail = NULL;
    pthread_mutex_unlock(&msg_mutex);
    return m;
}


/**
 * @brief Receives a line of text from the specified file descriptor.
 * 
 * @param fd The file descriptor to receive data from.
 * @param buf Pointer to the buffer to store the received line.
 * @param maxlen The maximum length of the buffer.
 * 
 * @return int The number of bytes received, or -1 on error.
 */
int recv_line(int fd, char *buf, size_t maxlen) {
    // ssize is for signed size
    // size_t is for unsigned size
    size_t idx = 0; // current index in buffer
    while (idx < maxlen - 1) {
        char c;
        ssize_t n = recv(fd, &c, 1, 0);
        if (n <= 0) return -1;
        buf[idx++] = c;
        if (c == '\n') break;
    }
    buf[idx] = '\0';
    return idx;
}

/**
 * @brief Adds a client to the global client list.
 * 
 * @param c Pointer to the client to add.
 */
void add_client(client_t *c) {
    pthread_mutex_lock(&clients_mutex);
    c->next = clients_head;
    clients_head = c;
    pthread_mutex_unlock(&clients_mutex);
}

/**
 * @brief Removes a client from the global client list.
 * 
 * @param c Pointer to the client, which we will remove.
 */
void remove_client(client_t *c) {
    pthread_mutex_lock(&clients_mutex);
    client_t **p = &clients_head;
    while (*p) {
        if (*p == c) {
            *p = c->next;
            break;
        }
        p = &(*p)->next;
    }
    pthread_mutex_unlock(&clients_mutex);
}

/**
 * @brief Checks if a username is already taken by a logged-in client.
 * 
 * @param username The username to check.
 * 
 * @return int 1 if taken, 0 if available.
 */
int username_taken(const char *username) {
    int taken = 0;
    pthread_mutex_lock(&clients_mutex);
    client_t *c = clients_head;
    while (c) {
        if (c->logged_in && strcmp(c->username, username) == 0) {
            taken = 1;
            break;
        }
        c = c->next;
    }
    pthread_mutex_unlock(&clients_mutex);
    return taken;
}

/**
 * @brief Closes and frees a client structure.
 * 
 * @param c Pointer to the client to close and free.
 */
void close_and_free_client(client_t *c) {
    if (!c) return;
    close(c->sockfd);
    remove_client(c);
    free(c);
}

/**
 * @brief Dispatcher thread function: dequeues messages and broadcasts them.
 * 
 * @param arg Unused parameter.
 */
void *dispatcher_thread(void *arg) {
    (void)arg; // For unused parameter warning
    while (server_running) {
        message_t *m = dequeue_message();
        if (!m) break;
        // Broadcast to all clients
        broadcast_formatted(m->sender, m->text);
        free(m);
    }
    return NULL;
}

/**
 * @brief Client thread function: handles communication with a connected client.
 * 
 * @param arg Pointer to the client structure.
 */
void *client_thread(void *arg) {
    client_t *c = (client_t *)arg;
    char buf[MAX_MESSAGE + 64];
    ssize_t n;

// ------------ PASSWORD PHASE WITH RETRIES -------------- //

    int attempts = 0;
    while (attempts < 5) {

        send_all(c->sockfd, "PASSWORD:\n", 10); // Prompt client

        if (recv_line(c->sockfd, buf, sizeof(buf)) <= 0) {
            close_and_free_client(c);
            return NULL;
        }

        // Trim newline
        char *nl = strchr(buf, '\n');
        if (nl) *nl = '\0';

        // Validate prefix
        if (strncmp(buf, "PASS:", 5) != 0) {
            send_all(c->sockfd, "ERR:Expected PASS:<password>\n", 30);
            attempts++;
            continue;
        }

        // Extract password text after PASS:
        const char *pw = buf + 5;

        // Check password
        if (strcmp(pw, SERVER_PASSWORD) == 0) {
            send_all(c->sockfd, "OKPASS\n", 7);
            break;  // SUCCESS
        }

        // Wrong password
        attempts++;
        send_all(c->sockfd, "ERR:Bad password\n", 17);
    }

    // Too many attempts?
    if (attempts >= 5) {
        send_all(c->sockfd, "ERR:Too many attempts\n", 23);
        close_and_free_client(c);
        return NULL;
    }


/*
// ------------ PASSWORD PHASE (RFC-CLEAN) -------------- //
    send_all(c->sockfd, "PASSWORD:\n", 10); // Prompt for password

    // Check to see if there was a password entered
    if (recv_line(c->sockfd, buf, sizeof(buf)) <= 0) {
        close_and_free_client(c);
        return NULL;
    }

    // Trim newline for fair comparison
    char *nl = strchr(buf, '\n');
    if (nl) *nl = '\0';

    // Make sure that the client has "PASS:" at the start of their password before actual comparison
    if (strncmp(buf, "PASS:", 5) != 0) {
        send_all(c->sockfd, "ERR:Expected PASS:<password>\n", 30);
        close_and_free_client(c);
        return NULL;
    }

    const char *pw = buf + 5; // Skip "PASS:"

    // Check password
    if (strcmp(pw, SERVER_PASSWORD) != 0) {
        send_all(c->sockfd, "ERR:Bad password\n", 17);
        return NULL;
    }
    

    // Password accepted
    send_all(c->sockfd, "OKPASS\n", 7);
*/
    

    // Expect LOGIN:<username>\n
    n = recv(c->sockfd, buf, sizeof(buf)-1, 0); // What the user types
    if (n <= 0) {
        close_and_free_client(c);
        return NULL;
    }
    buf[n] = '\0';

    // Trim newline for fair comparison
    char *newline = strchr(buf, '\n');
    if (newline) *newline = '\0';

    // Validate LOGIN format
    if (strncmp(buf, "LOGIN:", 6) != 0) {
        const char *err = "ERR:Invalid login. Send LOGIN:<username>\\n\n";
        send_all(c->sockfd, err, strlen(err));
        close_and_free_client(c);
        return NULL;
    }

    // Username buffer
    char uname[MAX_USERNAME]; 

    // Check username validity
    strncpy(uname, buf + 6, MAX_USERNAME-1);
    uname[MAX_USERNAME-1] = '\0';
    if (strlen(uname) == 0) {
        const char *err = "ERR:Empty username\n";
        send_all(c->sockfd, err, strlen(err));
        close_and_free_client(c);
        return NULL;
    }

    // Check to see if the username is already taken
    if (username_taken(uname)) {
        const char *err = "ERR:Username taken\n";
        send_all(c->sockfd, err, strlen(err));
        close_and_free_client(c);
        return NULL;
    }
    
    // Accept login
    strncpy(c->username, uname, MAX_USERNAME-1);
    c->logged_in = 1;
    send_all(c->sockfd, "OK\n", 3);

    // Announce join
    char joinmsg[MAX_MESSAGE];
    snprintf(joinmsg, sizeof(joinmsg), "*** %s has joined the chat ***", c->username);
    enqueue_message("Server", joinmsg);

    // Receive loop //
    char recvbuf[MAX_MESSAGE+1]; // Buffer for receiving messages
    size_t offset = 0;
    while (server_running) {
        n = recv(c->sockfd, recvbuf, sizeof(recvbuf)-1, 0);
        if (n <= 0) break; // If error or disconnect
        recvbuf[n] = '\0';

        // Line processing. Clients should send complete lines ending with \n
        char *p = recvbuf;
        // Process each line
        while (p && *p) {
            char *nl = strchr(p, '\n'); // Find newline with strchr, which returns pointer to first occurrence
            char line[MAX_MESSAGE+1]; // Buffer for a single line

            // Extract line
            if (nl) {
                size_t len = nl - p;
                if (len > MAX_MESSAGE-1) len = MAX_MESSAGE-1;
                memcpy(line, p, len);
                line[len] = '\0';
                p = nl + 1;
            } else { // No newline found, take the rest
                strncpy(line, p, MAX_MESSAGE-1);
                line[MAX_MESSAGE-1] = '\0';
                p += strlen(p);
            }

            // Process commands in the line sent by the client
            if (strncmp(line, "MSG:", 4) == 0) {
                enqueue_message(c->username, line + 4);
            } else if (strcmp(line, "QUIT") == 0) {
                goto disconnect;
            } else {
                // Unknown command, ignore or inform
                const char *err = "ERR:Unknown command\n";
                send_all(c->sockfd, err, strlen(err));
            }
        }
    }

disconnect:
    // Announce leave
    snprintf(joinmsg, sizeof(joinmsg), "*** %s has left the chat ***", c->username);
    enqueue_message("Server", joinmsg);
    close_and_free_client(c);
    return NULL;
}

/**
 * @brief Signal handler for SIGINT to gracefully shut down the server.
 * 
 * @param sig The signal number.
 */
void sigint_handler(int sig) {
    (void)sig;
    server_running = 0;
    if (server_sock >= 0) close(server_sock);
    // Wake dispatcher if waiting
    pthread_mutex_lock(&msg_mutex);
    pthread_cond_signal(&msg_cond);
    pthread_mutex_unlock(&msg_mutex);
}

int main(int argc, char **argv) {
    int port = DEFAULT_PORT;
    if (argc >= 2) port = atoi(argv[1]);

    signal(SIGINT, sigint_handler);
    signal(SIGPIPE, SIG_IGN);

    server_sock = socket(AF_INET, SOCK_STREAM, 0);
    if (server_sock < 0) {
        perror("socket");
        exit(1);
    }

    int opt = 1; // Enable address reuse
    setsockopt(server_sock, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    struct sockaddr_in srv;
    memset(&srv, 0, sizeof(srv));
    srv.sin_family = AF_INET;
    srv.sin_addr.s_addr = INADDR_ANY;
    srv.sin_port = htons(port);

    // Check to see if the binding was successful

    if (bind(server_sock, (struct sockaddr*)&srv, sizeof(srv)) < 0) {
        perror("bind");
        close(server_sock);
        exit(1);
    }

    if (listen(server_sock, 16) < 0) {
        perror("listen");
        close(server_sock);
        exit(1);
    }

    printf("Server listening on port %d\n", port);

    pthread_t dispatcher; // Dispatcher thread, which will handle message broadcasting
    pthread_create(&dispatcher, NULL, dispatcher_thread, NULL); // Start dispatcher thread

    // Accept loop for incoming client connections
    while (server_running) {
        struct sockaddr_in cliaddr; // Client address structure
        socklen_t addrlen = sizeof(cliaddr);
        int clientfd = accept(server_sock, (struct sockaddr*)&cliaddr, &addrlen);
        if (clientfd < 0) {
            if (errno == EINTR) continue;
            perror("accept");
            break;
        }

        // Create client structure
        client_t *c = calloc(1, sizeof(client_t));
        if (!c) {
            close(clientfd);
            continue;
        }
        c->sockfd = clientfd;
        c->logged_in = 0;
        c->next = NULL;
        add_client(c);

        pthread_t tid;
        if (pthread_create(&tid, NULL, client_thread, c) != 0) {
            perror("pthread_create");
            close_and_free_client(c);
            continue;
        }
        pthread_detach(tid);
    }

    // Shutdown: close all clients
    pthread_mutex_lock(&clients_mutex);
    client_t *it = clients_head;
    while (it) {
        close(it->sockfd);
        it = it->next;
    }
    pthread_mutex_unlock(&clients_mutex);

    // Wake dispatcher to exit
    pthread_mutex_lock(&msg_mutex);
    pthread_cond_signal(&msg_cond);
    pthread_mutex_unlock(&msg_mutex);

    pthread_join(dispatcher, NULL);

    printf("Server shutting down\n");
    return 0;
}
