// client.c
// Compile: gcc -pthread -o client client.c
// Run: ./client <server-ip> [port]
// Example: ./client 127.0.0.1 12345

// Include header files
#include <stdio.h> // for printf, fprintf, fgets, etc.
#include <stdlib.h> // for exit, atoi, etc.
#include <string.h> // for memset, strlen, strcmp, etc.
#include <unistd.h> // for close, read, write, etc.
#include <errno.h> // for errno
#include <signal.h> // for signal handling
#include <pthread.h> // for pthreads
#include <netinet/in.h> // for sockaddr_in
#include <sys/socket.h> // for socket functions
#include <arpa/inet.h> // for inet_pton

#define DEFAULT_PORT 12345
#define MAX_USERNAME 32
#define MAX_MESSAGE 1024

static int server_fd = -1;
static volatile int running = 1;

/**
 * @brief Sends all bytes in the buffer to the specified file descriptor.
 * 
 * @details This function attempts to send the entire buffer of length 'len' to fd.
 * 
 * @param fd The file descriptor to send data to.
 * @param buf Pointer to the buffer containing data to send.
 * @param len The length of the buffer in bytes.
 * 
 * @return ssize_t The total number of bytes sent, or -1 on error.
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
 * @brief Thread function to receive messages from the server.
 * 
 * @param arg Unused parameter.
 * 
 * @return void* Always returns NULL.
 */
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

// Helper: receive one line from server
    int recv_line_client(int fd, char *buf, size_t maxlen) {
        size_t idx = 0;
        while (idx < maxlen-1) {
            char c;
            ssize_t n = recv(fd, &c, 1, 0);
            if (n <= 0) return -1; // server closed or error
            buf[idx++] = c;
            if (c == '\n') break;
        }
        buf[idx] = '\0';
        return idx;
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

    // ---------------- PASSWORD PHASE (LINE-BY-LINE, MAX 5 ATTEMPTS) ---------------- //

    // Declare response buffer here
    char resp[256];
    int attempts = 0;

    while (attempts < 5) {
        // Wait for server prompt or message
        if (recv_line_client(server_fd, resp, sizeof(resp)) <= 0) {
            printf("Server closed connection.\n");
            close(server_fd);
            return 1;
        }

        // Print any error messages
        if (strncmp(resp, "PASSWORD:", 9) != 0) {
            printf("%s", resp);  // e.g., ERR:Bad password
            continue;            // wait for actual password prompt
        }

        // Prompt user
        char pw[128];
        printf("Enter server password: ");
        fflush(stdout);
        if (!fgets(pw, sizeof(pw), stdin)) {
            close(server_fd);
            return 1;
        }
        pw[strcspn(pw, "\n")] = '\0'; // remove newline

        // Send password to server
        char sendpw[256];
        snprintf(sendpw, sizeof(sendpw), "PASS:%s\n", pw);
        if (send_all(server_fd, sendpw, strlen(sendpw)) < 0) {
            perror("send");
            close(server_fd);
            return 1;
        }

        // Receive server response line
        if (recv_line_client(server_fd, resp, sizeof(resp)) <= 0) {
            printf("Server closed connection.\n");
            close(server_fd);
            return 1;
        }

        // Check response
        if (strncmp(resp, "OKPASS", 6) == 0) {
            printf("Password accepted.\n");
            break;
        }

        // Wrong password
        printf("%s", resp);
        attempts++;
    }

    // If max attempts reached
    if (attempts >= 5) {
        printf("Too many failed attempts. Disconnecting.\n");
        close(server_fd);
        return 1;
    }




/*
    // -------------- PASSWORD PHASE (RFC-CLEAN) -------------- //
    // Wait for PASSWORD:
    char resp[256];
    ssize_t n = recv(server_fd, resp, sizeof(resp)-1, 0);
    if (n <= 0) {
        perror("recv");
        close(server_fd);
        return 1;
    }
    resp[n] = '\0';

    // Check for "PASSWORD:"
    if (strncmp(resp, "PASSWORD:", 9) != 0) {
        printf("Unexpected server response: %s\n", resp);
        close(server_fd);
        return 1;
    }

    char pw[128]; // buffer for password

    printf("Enter server password: ");
    if (!fgets(pw, sizeof(pw), stdin)) {
        close(server_fd);
        return 1;
    }
    char *nlp = strchr(pw, '\n'); // Trim newline for fair comparison
    if (nlp) *nlp = '\0'; // Remove newline

    char sendpw[256]; // buffer for sending password
    snprintf(sendpw, sizeof(sendpw), "PASS:%s\n", pw); // Prepare PASS message
    send_all(server_fd, sendpw, strlen(sendpw)); // Send PASS:<password>\n

    n = recv(server_fd, resp, sizeof(resp)-1, 0); // Wait for server response
    if (n <= 0) {
        perror("recv");
        close(server_fd);
        return 1;
    }
    resp[n] = '\0';

    if (strncmp(resp, "OKPASS", 6) != 0) {
        printf("Server rejected password: %s\n", resp);
        
        close(server_fd);
        return 1;
    }
*/

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
    //char resp[256];
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
