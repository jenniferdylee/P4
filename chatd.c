#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <poll.h>
#include <netdb.h>
#include <ctype.h>
#include "protocol.h"

#define MAX_CLIENTS 100

typedef struct {
    int fd;
    char name[NAME_MAX_LEN + 1];
    char status[STATUS_MAX_LEN + 1];
    int active;
} User;

User clients[MAX_CLIENTS];
struct pollfd pfds[MAX_CLIENTS + 1];

int setup_server(const char *port) {
    struct addrinfo hints = {0}, *res;
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_flags = AI_PASSIVE;
    if (getaddrinfo(NULL, port, &hints, &res) != 0) exit(1);
    
    int server_fd = -1;
    for (struct addrinfo *r = res; r; r = r->ai_next) {
        server_fd = socket(r->ai_family, r->ai_socktype, r->ai_protocol);
        if (server_fd < 0) continue;
        int opt = 1;
        setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
        if (bind(server_fd, r->ai_addr, r->ai_addrlen) == 0) {
            if (listen(server_fd, 10) == 0) break;
        }
        close(server_fd);
        server_fd = -1;
    }
    freeaddrinfo(res);
    return server_fd;
}

void disconnect_client(int i) {
    close(clients[i].fd);
    clients[i].active = 0;
    clients[i].name[0] = '\0';
    clients[i].status[0] = '\0';
    pfds[i+1].fd = -1;
}

int check_name(const char *name) {
    int len = strlen(name);
    if (len < 1 || len > NAME_MAX_LEN) return ERR_TOO_LONG;
    for (int i = 0; i < len; i++) {
        if (!isalnum(name[i]) && name[i] != '-' && name[i] != '_') return ERR_ILLEGAL_CHR;
    }
    for (int i = 0; i < MAX_CLIENTS; i++) {
        if (clients[i].active && strcmp(clients[i].name, name) == 0) return ERR_NAME_IN_USE;
    }
    return -1;
}

int check_status(const char *status) {
    int len = strlen(status);
    if (len > STATUS_MAX_LEN) return ERR_TOO_LONG;
    for (int i = 0; i < len; i++) {
        if ((unsigned char)status[i] < 32 || (unsigned char)status[i] > 126) return ERR_ILLEGAL_CHR;
    }
    return -1;
}

int check_msg_body(const char *body) {
    int len = strlen(body);
    if (len < 1 || len > MSG_MAX_LEN) return ERR_TOO_LONG;
    for (int i = 0; i < len; i++) {
        if ((unsigned char)body[i] < 32 || (unsigned char)body[i] > 126) return ERR_ILLEGAL_CHR;
    }
    return -1;
}

void send_err(int fd, int code, const char *msg) {
    char code_str[16];
    sprintf(code_str, "%d", code);
    const char *fields[] = { code_str, msg, NULL };
    send_message(fd, CODE_ERR, fields);
}

void process_msg(int i, Message *msg) {
    if (strcmp(msg->code, CODE_NAM) == 0) {
        if (msg->nfields < 1) {
            send_err(clients[i].fd, ERR_UNREADABLE, "Missing name");
            disconnect_client(i);
            return;
        }
        int err = check_name(msg->fields[0]);
        if (err != -1) {
            send_err(clients[i].fd, err, "Invalid name");
            return;
        }
        strcpy(clients[i].name, msg->fields[0]);
        const char *fields[] = { "#all", msg->fields[0], "Welcome to the chat!", NULL };
        send_message(clients[i].fd, CODE_MSG, fields);
    } else if (strcmp(msg->code, CODE_SET) == 0) {
        if (msg->nfields < 1) {
            send_err(clients[i].fd, ERR_UNREADABLE, "Missing status");
            return;
        }
        int err = check_status(msg->fields[0]);
        if (err != -1) {
            send_err(clients[i].fd, err, "Invalid status");
            return;
        }
        strcpy(clients[i].status, msg->fields[0]);
        if (strlen(clients[i].status) > 0) {
            char buf[256];
            snprintf(buf, sizeof(buf), "%s is now \"%s\"", clients[i].name, clients[i].status);
            const char *fields[] = { "#all", "#all", buf, NULL };
            for (int j = 0; j < MAX_CLIENTS; j++) {
                if (clients[j].active && clients[j].name[0] != '\0') {
                    send_message(clients[j].fd, CODE_MSG, fields);
                }
            }
        }
    } else if (strcmp(msg->code, CODE_MSG) == 0) {
        if (msg->nfields < 3) {
            send_err(clients[i].fd, ERR_UNREADABLE, "Bad MSG");
            return;
        }
        int err = check_msg_body(msg->fields[2]);
        if (err != -1) {
            send_err(clients[i].fd, err, "Invalid message");
            return;
        }
        const char *recip = msg->fields[1];
        if (strcmp(recip, "#all") == 0) {
            const char *fields[] = { clients[i].name, "#all", msg->fields[2], NULL };
            for (int j = 0; j < MAX_CLIENTS; j++) {
                if (clients[j].active && clients[j].name[0] != '\0' && j != i) {
                    send_message(clients[j].fd, CODE_MSG, fields);
                }
            }
        } else {
            int found = -1;
            for (int j = 0; j < MAX_CLIENTS; j++) {
                if (clients[j].active && strcmp(clients[j].name, recip) == 0) {
                    found = j;
                    break;
                }
            }
            if (found == -1) {
                send_err(clients[i].fd, ERR_UNKNOWN_RCP, "User not found");
            } else {
                const char *fields[] = { clients[i].name, recip, msg->fields[2], NULL };
                send_message(clients[found].fd, CODE_MSG, fields);
            }
        }
    } else if (strcmp(msg->code, CODE_WHO) == 0) {
        if (msg->nfields < 1) {
            send_err(clients[i].fd, ERR_UNREADABLE, "Missing target");
            return;
        }
        const char *target = msg->fields[0];
        char out[8192] = {0};
        if (strcmp(target, "#all") == 0) {
            for (int j = 0; j < MAX_CLIENTS; j++) {
                if (clients[j].active && clients[j].name[0] != '\0') {
                    if (strlen(clients[j].status) > 0) {
                        snprintf(out + strlen(out), sizeof(out) - strlen(out), "%s: %s\n", clients[j].name, clients[j].status);
                    } else {
                        snprintf(out + strlen(out), sizeof(out) - strlen(out), "%s\n", clients[j].name);
                    }
                }
            }
            if (strlen(out) > 0) out[strlen(out)-1] = '\0';
            const char *fields[] = { "#all", clients[i].name, out, NULL };
            send_message(clients[i].fd, CODE_MSG, fields);
        } else {
            int found = -1;
            for (int j = 0; j < MAX_CLIENTS; j++) {
                if (clients[j].active && strcmp(clients[j].name, target) == 0) {
                    found = j;
                    break;
                }
            }
            if (found == -1) {
                send_err(clients[i].fd, ERR_UNKNOWN_RCP, "User not found");
            } else {
                if (strlen(clients[found].status) > 0) {
                    snprintf(out, sizeof(out), "%s: %s", clients[found].name, clients[found].status);
                } else {
                    snprintf(out, sizeof(out), "No status");
                }
                const char *fields[] = { "#all", clients[i].name, out, NULL };
                send_message(clients[i].fd, CODE_MSG, fields);
            }
        }
    } else {
        send_err(clients[i].fd, ERR_UNREADABLE, "Unknown type");
        disconnect_client(i);
    }
}

int main(int argc, char *argv[]) {
    if (argc != 2) exit(1);
    int server_fd = setup_server(argv[1]);
    if (server_fd < 0) exit(1);

    for (int i = 0; i < MAX_CLIENTS; i++) {
        clients[i].active = 0;
        pfds[i+1].fd = -1;
    }
    pfds[0].fd = server_fd;
    pfds[0].events = POLLIN;

    while (1) {
        if (poll(pfds, MAX_CLIENTS + 1, -1) < 0) continue;
        if (pfds[0].revents & POLLIN) {
            struct sockaddr_storage remoteaddr;
            socklen_t addrlen = sizeof(remoteaddr);
            int newfd = accept(server_fd, (struct sockaddr *)&remoteaddr, &addrlen);
            if (newfd >= 0) {
                int slot = -1;
                for (int i = 0; i < MAX_CLIENTS; i++) {
                    if (!clients[i].active) {
                        slot = i;
                        break;
                    }
                }
                if (slot != -1) {
                    clients[slot].fd = newfd;
                    clients[slot].active = 1;
                    clients[slot].name[0] = '\0';
                    clients[slot].status[0] = '\0';
                    pfds[slot+1].fd = newfd;
                    pfds[slot+1].events = POLLIN;
                } else {
                    close(newfd);
                }
            }
        }
        for (int i = 0; i < MAX_CLIENTS; i++) {
            if (clients[i].active && (pfds[i+1].revents & POLLIN)) {
                Message msg;
                if (recv_message(clients[i].fd, &msg) < 0) {
                    disconnect_client(i);
                } else {
                    process_msg(i, &msg);
                }
            }
        }
    }
    return 0;
}
