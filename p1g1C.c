// client.c
// Compile: gcc -pthread -o client client.c
// Run: ./client <server-ip> [port]
// Example: ./client 127.0.0.1 12345


#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <signal.h>
#include <pthread.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <arpa/inet.h>

#define DEFAULT_PORT 12345
#define MAX_USERNAME 32
#define MAX_MESSAGE 1024

static int server_fd = -1;
static volatile int running = 1;

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

void *recv_thread(void *arg) {
    (void)arg;
    char buf[2048];
    while (running) {
        ssize_t n = recv(server_fd, buf, sizeof(buf)-1, 0);
        if (n <= 0) {
            if (n == 0) {
                printf("\n[Disconnected from server]\n");
            } else {
                perror("recv");
            }
            running = 0;
            break;
        }
        buf[n] = '\0';
        // Print server message
        fputs(buf, stdout);
        fflush(stdout);
    }
    return NULL;
}

int main(int argc, char **argv) {
    if (argc < 2) {
        fprintf(stderr, "Usage: %s <server-ip> [port]\n", argv[0]);
        return 1;
    }
    const char *server_ip = argv[1];
    int port = DEFAULT_PORT;
    if (argc >= 3) port = atoi(argv[2]);

    signal(SIGINT, SIG_IGN);

    server_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (server_fd < 0) {
        perror("socket");
        return 1;
    }

    struct sockaddr_in srv;
    memset(&srv, 0, sizeof(srv));
    srv.sin_family = AF_INET;
    srv.sin_port = htons(port);
    if (inet_pton(AF_INET, server_ip, &srv.sin_addr) <= 0) {
        fprintf(stderr, "Invalid server IP\n");
        close(server_fd);
        return 1;
    }

    if (connect(server_fd, (struct sockaddr*)&srv, sizeof(srv)) < 0) {
        perror("connect");
        close(server_fd);
        return 1;
    }

    // Prompt for username
    char username[MAX_USERNAME];
    printf("Enter username: ");
    if (!fgets(username, sizeof(username), stdin)) {
        close(server_fd);
        return 1;
    }
    // trim newline
    char *nl = strchr(username, '\n');
    if (nl) *nl = '\0';
    if (strlen(username) == 0) {
        printf("Empty username\n");
        close(server_fd);
        return 1;
    }

    // Send LOGIN:<username>\n
    char login_msg[128];
    snprintf(login_msg, sizeof(login_msg), "LOGIN:%s\n", username);
    if (send_all(server_fd, login_msg, strlen(login_msg)) < 0) {
        perror("send");
        close(server_fd);
        return 1;
    }

    // Wait for server response (OK or ERR:)
    char resp[256];
    ssize_t nr = recv(server_fd, resp, sizeof(resp)-1, 0);
    if (nr <= 0) {
        perror("recv");
        close(server_fd);
        return 1;
    }
    resp[nr] = '\0';
    if (strncmp(resp, "OK", 2) == 0) {
        printf("[Connected to chat as '%s']\n", username);
    } else {
        printf("Server response: %s\n", resp);
        close(server_fd);
        return 1;
    }

    // Start receive thread
    pthread_t rt;
    pthread_create(&rt, NULL, recv_thread, NULL);
    pthread_detach(rt);

    // Input loop: read stdin lines and send MSG:<text>\n
    char line[MAX_MESSAGE];
    while (running && fgets(line, sizeof(line), stdin)) {
        // if user types /quit or /exit, send QUIT and break
        if (strncmp(line, "/quit", 5) == 0 || strncmp(line, "/exit", 5) == 0) {
            send_all(server_fd, "QUIT\n", 5);
            break;
        }
        // Trim newline
        char *p = strchr(line, '\n');
        if (p) *p = '\0';

        char out[MAX_MESSAGE + 8];
        snprintf(out, sizeof(out), "MSG:%s\n", line);
        if (send_all(server_fd, out, strlen(out)) < 0) {
            perror("send");
            break;
        }
    }

    running = 0;
    close(server_fd);
    printf("Closed connection\n");
    return 0;
}
