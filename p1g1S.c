#define _POSIX_C_SOURCE 200112L
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

typedef struct client {
    int sockfd;
    char username[MAX_USERNAME];
    int logged_in; // 0 = not logged in yet
    struct client *next;
} client_t;

typedef struct message {
    char sender[MAX_USERNAME];
    char text[MAX_MESSAGE];
    struct message *next;
} message_t;

// Global client list (linked list)
static client_t *clients_head = NULL;
static pthread_mutex_t clients_mutex = PTHREAD_MUTEX_INITIALIZER;

// Message queue (linked list)
static message_t *msg_head = NULL;
static message_t *msg_tail = NULL;
static pthread_mutex_t msg_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t msg_cond = PTHREAD_COND_INITIALIZER;

static int server_sock = -1;
static volatile int server_running = 1;

// Utility: safe send all bytes
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

void broadcast_formatted(const char *sender, const char *text) {
    // format: username: text\n
    char out[MAX_USERNAME + 2 + MAX_MESSAGE + 2];
    snprintf(out, sizeof(out), "%s: %s\n", sender, text);

    pthread_mutex_lock(&clients_mutex);
    client_t *c = clients_head;
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

// Message queue operations
void enqueue_message(const char *sender, const char *text) {
    message_t *m = calloc(1, sizeof(message_t));
    if (!m) return;
    strncpy(m->sender, sender, MAX_USERNAME-1);
    strncpy(m->text, text, MAX_MESSAGE-1);
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

// Client list operations
void add_client(client_t *c) {
    pthread_mutex_lock(&clients_mutex);
    c->next = clients_head;
    clients_head = c;
    pthread_mutex_unlock(&clients_mutex);
}

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

// Check if username already taken
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

void close_and_free_client(client_t *c) {
    if (!c) return;
    close(c->sockfd);
    remove_client(c);
    free(c);
}

// Dispatcher thread: pops messages and broadcasts
void *dispatcher_thread(void *arg) {
    (void)arg;
    while (server_running) {
        message_t *m = dequeue_message();
        if (!m) break;
        // Broadcast to all clients
        broadcast_formatted(m->sender, m->text);
        free(m);
    }
    return NULL;
}

// Per-client thread: handle login, receive messages, enqueue them
void *client_thread(void *arg) {
    client_t *c = (client_t *)arg;
    char buf[MAX_MESSAGE + 64];
    ssize_t n;

    // Expect LOGIN:<username>\n
    n = recv(c->sockfd, buf, sizeof(buf)-1, 0);
    if (n <= 0) {
        close_and_free_client(c);
        return NULL;
    }
    buf[n] = '\0';

    // Trim newline
    char *newline = strchr(buf, '\n');
    if (newline) *newline = '\0';

    if (strncmp(buf, "LOGIN:", 6) != 0) {
        const char *err = "ERR:Invalid login. Send LOGIN:<username>\\n\n";
        send_all(c->sockfd, err, strlen(err));
        close_and_free_client(c);
        return NULL;
    }
    char uname[MAX_USERNAME];
    strncpy(uname, buf + 6, MAX_USERNAME-1);
    uname[MAX_USERNAME-1] = '\0';
    if (strlen(uname) == 0) {
        const char *err = "ERR:Empty username\n";
        send_all(c->sockfd, err, strlen(err));
        close_and_free_client(c);
        return NULL;
    }
    if (username_taken(uname)) {
        const char *err = "ERR:Username taken\n";
        send_all(c->sockfd, err, strlen(err));
        close_and_free_client(c);
        return NULL;
    }

    // Accept
    strncpy(c->username, uname, MAX_USERNAME-1);
    c->logged_in = 1;
    send_all(c->sockfd, "OK\n", 3);

    // Announce join
    char joinmsg[MAX_MESSAGE];
    snprintf(joinmsg, sizeof(joinmsg), "*** %s has joined the chat ***", c->username);
    enqueue_message("Server", joinmsg);

    // Receive loop
    char recvbuf[MAX_MESSAGE+1];
    size_t offset = 0;
    while (server_running) {
        n = recv(c->sockfd, recvbuf, sizeof(recvbuf)-1, 0);
        if (n <= 0) break;
        recvbuf[n] = '\0';
        // handle line-by-line: the client should send lines ending in '\n'
        char *p = recvbuf;
        while (p && *p) {
            char *nl = strchr(p, '\n');
            char line[MAX_MESSAGE+1];
            if (nl) {
                size_t len = nl - p;
                if (len > MAX_MESSAGE-1) len = MAX_MESSAGE-1;
                memcpy(line, p, len);
                line[len] = '\0';
                p = nl + 1;
            } else {
                strncpy(line, p, MAX_MESSAGE-1);
                line[MAX_MESSAGE-1] = '\0';
                p += strlen(p);
            }

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

    int opt = 1;
    setsockopt(server_sock, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    struct sockaddr_in srv;
    memset(&srv, 0, sizeof(srv));
    srv.sin_family = AF_INET;
    srv.sin_addr.s_addr = INADDR_ANY;
    srv.sin_port = htons(port);

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

    pthread_t dispatcher;
    pthread_create(&dispatcher, NULL, dispatcher_thread, NULL);

    while (server_running) {
        struct sockaddr_in cliaddr;
        socklen_t addrlen = sizeof(cliaddr);
        int clientfd = accept(server_sock, (struct sockaddr*)&cliaddr, &addrlen);
        if (clientfd < 0) {
            if (errno == EINTR) continue;
            perror("accept");
            break;
        }

        // create client struct
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
