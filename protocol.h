#ifndef PROTOCOL_H
#define PROTOCOL_H

#include <stddef.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/socket.h>
#include <errno.h>

/* ── limits from spec ── */
#define PROTO_VERSION   "1"
#define NAME_MAX_LEN    32
#define STATUS_MAX_LEN  64
#define MSG_MAX_LEN     80
#define LEN_FIELD_DIGITS 5          
#define BUF_SIZE        (1 << 17)   

/* ── message codes ── */
#define CODE_NAM "NAM"
#define CODE_SET "SET"
#define CODE_MSG "MSG"
#define CODE_WHO "WHO"
#define CODE_ERR "ERR"

/* ── error codes ── */
#define ERR_UNREADABLE  0
#define ERR_NAME_IN_USE 1
#define ERR_UNKNOWN_RCP 2
#define ERR_ILLEGAL_CHR 3
#define ERR_TOO_LONG    4

/* ── parsed message structure ── */
typedef struct {
    char code[4];           /* e.g. "MSG" */
    char fields[8][512];    /* up to 8 fields, each up to 511 chars */
    int  nfields;
} Message;

static inline int send_all(int fd, const char *buf, size_t n) {
    size_t sent = 0;
    while (sent < n) {
        ssize_t r = send(fd, buf + sent, n - sent, 0);
        if (r <= 0) return -1;
        sent += (size_t)r;
    }
    return 0;
}

static inline int recv_exact(int fd, char *buf, size_t n) {
    size_t got = 0;
    while (got < n) {
        ssize_t r = recv(fd, buf + got, n - got, 0);
        if (r <= 0) return -1;
        got += (size_t)r;
    }
    return 0;
}


static inline int read_until_bar(int fd, char *out, int max) {
    int i = 0;
    while (1) {
        char c;
        ssize_t r = recv(fd, &c, 1, 0);
        if (r <= 0) return -1;
        if (c == '|') { out[i] = '\0'; return i; }
        if (i < max - 1) out[i++] = c;
        /* if too long we keep reading to stay in sync but discard */
    }
}


static inline int recv_message(int fd, Message *msg) {
    char tmp[64];

    /* version field */
    if (read_until_bar(fd, tmp, sizeof(tmp)) < 0) return -1;

    /* code field */
    if (read_until_bar(fd, msg->code, sizeof(msg->code)) < 0) return -1;

    /* body-length field */
    if (read_until_bar(fd, tmp, sizeof(tmp)) < 0) return -1;
    int bodylen = atoi(tmp);
    if (bodylen < 0 || bodylen > 99999) return -1;

    /* read exactly bodylen bytes */
    char *body = malloc(bodylen + 1);
    if (!body) return -1;
    if (bodylen > 0 && recv_exact(fd, body, bodylen) < 0) {
        free(body); return -1;
    }
    body[bodylen] = '\0';

    /* split body on '|' into fields */
    msg->nfields = 0;
    char *p = body;
    char *end = body + bodylen;
    while (p < end && msg->nfields < 8) {
        char *bar = memchr(p, '|', end - p);
        if (!bar) break;
        int flen = (int)(bar - p);
        if (flen >= 512) flen = 511;
        memcpy(msg->fields[msg->nfields], p, flen);
        msg->fields[msg->nfields][flen] = '\0';
        msg->nfields++;
        p = bar + 1;
    }

    free(body);
    return 0;
}

static inline int build_message(char *buf, int bufsz,
                                const char *code,
                                const char **fields) {
    char body[BUF_SIZE];
    int blen = 0;
    for (int i = 0; fields[i] != NULL; i++) {
        int flen = (int)strlen(fields[i]);
        if (blen + flen + 1 >= (int)sizeof(body)) return -1;
        memcpy(body + blen, fields[i], flen);
        blen += flen;
        body[blen++] = '|';
    }
    body[blen] = '\0';

    int n = snprintf(buf, bufsz, "%s|%s|%d|%s",
                     PROTO_VERSION, code, blen, body);
    if (n < 0 || n >= bufsz) return -1;
    return n;
}


static inline int send_message(int fd, const char *code,
                               const char **fields) {
    char buf[BUF_SIZE];
    int n = build_message(buf, sizeof(buf), code, fields);
    if (n < 0) return -1;
    return send_all(fd, buf, n);
}

#endif /* PROTOCOL_H */
