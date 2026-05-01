#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>
#include <netdb.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#include "protocol.h"

/* shared flag: set to 1 when either thread wants to quit */
static volatile int done = 0;

/* ── helpers ── */

static void trim_newline(char *s) {
    size_t l = strlen(s);
    if (l > 0 && s[l-1] == '\n') s[l-1] = '\0';
    if (l > 1 && s[l-2] == '\r') s[l-2] = '\0';
}


static void print_server_msg(const Message *msg) {
    if (msg->nfields < 3) {
        printf("[malformed MSG from server]\n");
        return;
    }
    const char *sender = msg->fields[0];
    const char *recip  = msg->fields[1];
    const char *body   = msg->fields[2];

    if (strcmp(sender, "#all") == 0) {
        /* server announcement */
        printf("[server] %s\n", body);
    } else if (strcmp(recip, "#all") == 0) {
        printf("<%s> %s\n", sender, body);
    } else {
        printf("[PM from %s] %s\n", sender, body);
    }
    fflush(stdout);
}

/* ── receiver thread ── */

static void *receiver_thread(void *arg) {
    int sockfd = *(int *)arg;
    Message msg;

    while (!done) {
        if (recv_message(sockfd, &msg) < 0) {
            if (!done) {
                printf("\n[disconnected from server]\n");
            }
            done = 1;
            break;
        }

        if (strcmp(msg.code, CODE_MSG) == 0) {
            print_server_msg(&msg);
        } else if (strcmp(msg.code, CODE_ERR) == 0) {
            int code = msg.nfields >= 1 ? atoi(msg.fields[0]) : -1;
            const char *explanation = msg.nfields >= 2 ? msg.fields[1] : "?";
            printf("[error %d] %s\n", code, explanation);
            fflush(stdout);
            /* error 0 is fatal */
            if (code == ERR_UNREADABLE) {
                done = 1;
                break;
            }
        } else {
            printf("[unexpected message type: %s]\n", msg.code);
            fflush(stdout);
        }
    }
    return NULL;
}

/* ── stdin sender loop ── */

static void sender_loop(int sockfd) {
    char line[512];

    printf("Commands:\n");
    printf("  /msg <recipient> <text>  – send a message\n");
    printf("  /who <name|#all>         – query user info\n");
    printf("  /set <status>            – set your status\n");
    printf("  /quit                    – disconnect\n");
    printf("  <anything else>          – send to #all\n\n");

    while (!done) {
        printf("> ");
        fflush(stdout);

        if (!fgets(line, sizeof(line), stdin)) {
            /* EOF (Ctrl-D) */
            done = 1;
            break;
        }
        trim_newline(line);
        if (strlen(line) == 0) continue;

        if (strcmp(line, "/quit") == 0) {
            done = 1;
            break;

        } else if (strncmp(line, "/msg ", 5) == 0) {
            /* /msg <recipient> <text> */
            char *rest = line + 5;
            char *space = strchr(rest, ' ');
            if (!space) {
                printf("[usage: /msg <recipient> <text>]\n");
                continue;
            }
            *space = '\0';
            const char *recip = rest;
            const char *body  = space + 1;
            const char *fields[] = { "", recip, body, NULL };
            if (send_message(sockfd, CODE_MSG, fields) < 0) {
                printf("[send error]\n"); done = 1; break;
            }

        } else if (strncmp(line, "/who ", 5) == 0) {
            const char *target = line + 5;
            const char *fields[] = { target, NULL };
            if (send_message(sockfd, CODE_WHO, fields) < 0) {
                printf("[send error]\n"); done = 1; break;
            }

        } else if (strncmp(line, "/set ", 5) == 0) {
            const char *status = line + 5;
            const char *fields[] = { status, NULL };
            if (send_message(sockfd, CODE_SET, fields) < 0) {
                printf("[send error]\n"); done = 1; break;
            }

        } else {
            /* bare text → send to #all */
            const char *fields[] = { "", "#all", line, NULL };
            if (send_message(sockfd, CODE_MSG, fields) < 0) {
                printf("[send error]\n"); done = 1; break;
            }
        }
    }
}

/* ── main ── */

int main(int argc, char *argv[]) {
    if (argc != 3) {
        fprintf(stderr, "usage: %s <hostname> <port>\n", argv[0]);
        return 1;
    }

    const char *hostname = argv[1];
    const char *port     = argv[2];

    /* resolve hostname */
    struct addrinfo hints = {0}, *res;
    hints.ai_family   = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    int err = getaddrinfo(hostname, port, &hints, &res);
    if (err) {
        fprintf(stderr, "getaddrinfo: %s\n", gai_strerror(err));
        return 1;
    }

    /* connect */
    int sockfd = -1;
    for (struct addrinfo *r = res; r; r = r->ai_next) {
        sockfd = socket(r->ai_family, r->ai_socktype, r->ai_protocol);
        if (sockfd < 0) continue;
        if (connect(sockfd, r->ai_addr, r->ai_addrlen) == 0) break;
        close(sockfd);
        sockfd = -1;
    }
    freeaddrinfo(res);

    if (sockfd < 0) {
        perror("connect");
        return 1;
    }
    printf("Connected to %s:%s\n", hostname, port);

    /* choose screen name */
    char name[NAME_MAX_LEN + 2];
    while (1) {
        printf("Screen name: ");
        fflush(stdout);
        if (!fgets(name, sizeof(name), stdin)) { close(sockfd); return 0; }
        trim_newline(name);
        if (strlen(name) == 0) { printf("Name cannot be empty.\n"); continue; }
        if (strlen(name) > NAME_MAX_LEN) {
            printf("Name too long (max %d chars).\n", NAME_MAX_LEN);
            continue;
        }
        /* send NAM */
        const char *fields[] = { name, NULL };
        if (send_message(sockfd, CODE_NAM, fields) < 0) {
            perror("send"); close(sockfd); return 1;
        }
        /* wait for server reply */
        Message reply;
        if (recv_message(sockfd, &reply) < 0) {
            printf("Server closed connection.\n"); close(sockfd); return 1;
        }
        if (strcmp(reply.code, CODE_MSG) == 0) {
            /* welcome */
            if (reply.nfields >= 3) printf("[server] %s\n", reply.fields[2]);
            break;
        } else if (strcmp(reply.code, CODE_ERR) == 0) {
            int code = reply.nfields >= 1 ? atoi(reply.fields[0]) : -1;
            const char *expl = reply.nfields >= 2 ? reply.fields[1] : "?";
            printf("[error %d] %s\n", code, expl);
            if (code == ERR_UNREADABLE) { close(sockfd); return 1; }
            /* recoverable: try again */
        }
    }

    /* spin up receiver thread */
    pthread_t tid;
    if (pthread_create(&tid, NULL, receiver_thread, &sockfd) != 0) {
        perror("pthread_create"); close(sockfd); return 1;
    }
    pthread_detach(tid);

    /* sender loop (runs in main thread) */
    sender_loop(sockfd);

    close(sockfd);
    return 0;
}

