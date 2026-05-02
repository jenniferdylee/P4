#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>
#include <netdb.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#include "protocol.h"


static int pass_count = 0;
static int fail_count = 0;

#define PASS(label) do { printf("  [PASS] %s\n", label); pass_count++; } while(0)
#define FAIL(label, reason) do { \
    printf("  [FAIL] %s: %s\n", label, reason); fail_count++; } while(0)

static int connect_to(const char *host, const char *port) {
    struct addrinfo hints = {0}, *res;
    hints.ai_family   = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    if (getaddrinfo(host, port, &hints, &res) != 0) return -1;

    int fd = -1;
    for (struct addrinfo *r = res; r; r = r->ai_next) {
        fd = socket(r->ai_family, r->ai_socktype, r->ai_protocol);
        if (fd < 0) continue;
        if (connect(fd, r->ai_addr, r->ai_addrlen) == 0) break;
        close(fd); fd = -1;
    }
    freeaddrinfo(res);
    return fd;
}

/* send NAM and return 1 if welcome MSG received, 0 if ERR received */
static int do_login(int fd, const char *name, int *err_code_out) {
    const char *fields[] = { name, NULL };
    if (send_message(fd, CODE_NAM, fields) < 0) return -1;
    Message msg;
    if (recv_message(fd, &msg) < 0) return -1;
    if (strcmp(msg.code, CODE_MSG) == 0) return 1;   /* welcome */
    if (strcmp(msg.code, CODE_ERR) == 0) {
        if (err_code_out) *err_code_out = atoi(msg.fields[0]);
        return 0;
    }
    return -1;
}

static int recv_message_timeout(int fd, Message *msg, int secs) {
    fd_set rfds;
    FD_ZERO(&rfds);
    FD_SET(fd, &rfds);
    struct timeval tv = { secs, 0 };
    int r = select(fd + 1, &rfds, NULL, NULL, &tv);
    if (r <= 0) return -1;   /* timeout or error */
    return recv_message(fd, msg);
}

static void drain(int fd) {
    Message m;
    while (recv_message_timeout(fd, &m, 1) == 0) { /* discard */ }
}

/* ── individual tests ── */

static void t01_normal_login(const char *host, const char *port) {
    printf("T01: Normal login\n");
    int fd = connect_to(host, port);
    if (fd < 0) { FAIL("connect", "could not connect"); return; }

    int r = do_login(fd, "Alice", NULL);
    if (r == 1) PASS("welcome MSG received");
    else        FAIL("welcome MSG", "did not get MSG after NAM");

    close(fd);
}

static void t02_duplicate_name(const char *host, const char *port) {
    printf("T02: Duplicate screen name\n");
    int fd1 = connect_to(host, port);
    int fd2 = connect_to(host, port);
    if (fd1 < 0 || fd2 < 0) {
        FAIL("connect", "could not open two connections");
        if (fd1 >= 0) close(fd1);
        if (fd2 >= 0) close(fd2);
        return;
    }
    do_login(fd1, "Alice", NULL);

    int err = -1;
    int r = do_login(fd2, "Alice", &err);
    if (r == 0 && err == ERR_NAME_IN_USE)
        PASS("error 1 (name in use) returned");
    else
        FAIL("error 1", "expected ERR code 1");

    close(fd1);
    close(fd2);
}

static void t03_name_too_long(const char *host, const char *port) {
    printf("T03: Screen name too long\n");
    int fd = connect_to(host, port);
    if (fd < 0) { FAIL("connect", "could not connect"); return; }

    /* 33 chars – one over the limit */
    char longname[34];
    memset(longname, 'A', 33);
    longname[33] = '\0';

    int err = -1;
    int r = do_login(fd, longname, &err);
    if (r == 0 && err == ERR_TOO_LONG)
        PASS("error 4 (too long) returned");
    else
        FAIL("error 4", "expected ERR code 4");

    close(fd);
}

static void t04_illegal_char_in_name(const char *host, const char *port) {
    printf("T04: Illegal character in name\n");
    int fd = connect_to(host, port);
    if (fd < 0) { FAIL("connect", "could not connect"); return; }

    int err = -1;
    /* space is not allowed in a name */
    int r = do_login(fd, "Bad Name", &err);
    if (r == 0 && err == ERR_ILLEGAL_CHR)
        PASS("error 3 (illegal char) returned");
    else
        FAIL("error 3", "expected ERR code 3");

    close(fd);
}

static void t05_broadcast_msg(const char *host, const char *port) {
    printf("T05: Broadcast MSG delivered to others\n");
    int sender = connect_to(host, port);
    int recver = connect_to(host, port);
    if (sender < 0 || recver < 0) {
        FAIL("connect", "could not open two connections");
        if (sender >= 0) close(sender);
        if (recver >= 0) close(recver);
        return;
    }
    do_login(sender, "Alice", NULL);
    do_login(recver, "Bob",   NULL);
    drain(recver);   /* discard join announcements */

    const char *fields[] = { "", "#all", "Hello everyone!", NULL };
    send_message(sender, CODE_MSG, fields);

    Message m;
    if (recv_message_timeout(recver, &m, 3) == 0 &&
        strcmp(m.code, CODE_MSG) == 0 &&
        m.nfields >= 3 &&
        strcmp(m.fields[0], "Alice") == 0 &&
        strcmp(m.fields[1], "#all")  == 0 &&
        strstr(m.fields[2], "Hello everyone!") != NULL)
        PASS("broadcast received with correct sender/recipient");
    else
        FAIL("broadcast", "did not receive expected MSG");

    close(sender);
    close(recver);
}

static void t06_private_msg(const char *host, const char *port) {
    printf("T06: Private MSG delivered only to target\n");
    int alice = connect_to(host, port);
    int bob   = connect_to(host, port);
    int carol = connect_to(host, port);
    if (alice < 0 || bob < 0 || carol < 0) {
        FAIL("connect", "could not open three connections");
        if (alice >= 0) close(alice);
        if (bob   >= 0) close(bob);
        if (carol >= 0) close(carol);
        return;
    }
    do_login(alice, "Alice", NULL);
    do_login(bob,   "Bob",   NULL);
    do_login(carol, "Carol", NULL);
    drain(bob);
    drain(carol);

    /* Alice sends private to Bob */
    const char *fields[] = { "", "Bob", "Psst, Bob!", NULL };
    send_message(alice, CODE_MSG, fields);

    /* Bob should receive it */
    Message m;
    int bob_got = (recv_message_timeout(bob, &m, 3) == 0 &&
                   strcmp(m.code, CODE_MSG) == 0 &&
                   m.nfields >= 3 &&
                   strcmp(m.fields[0], "Alice") == 0 &&
                   strcmp(m.fields[1], "Bob")   == 0);
    if (bob_got) PASS("target received private MSG");
    else         FAIL("private MSG target", "Bob did not get the message");

    /* Carol should NOT receive it */
    int carol_got = (recv_message_timeout(carol, &m, 1) == 0 &&
                     strcmp(m.code, CODE_MSG) == 0 &&
                     m.nfields >= 3 &&
                     strcmp(m.fields[0], "Alice") == 0);
    if (!carol_got) PASS("non-target did NOT receive private MSG");
    else            FAIL("private MSG isolation", "Carol received a message she shouldn't have");

    close(alice); close(bob); close(carol);
}

static void t07_unknown_recipient(const char *host, const char *port) {
    printf("T07: MSG to unknown recipient\n");
    int fd = connect_to(host, port);
    if (fd < 0) { FAIL("connect", "could not connect"); return; }
    do_login(fd, "Alice", NULL);
    drain(fd);

    const char *fields[] = { "", "NoSuchUser", "hello?", NULL };
    send_message(fd, CODE_MSG, fields);

    Message m;
    if (recv_message_timeout(fd, &m, 3) == 0 &&
        strcmp(m.code, CODE_ERR) == 0 &&
        m.nfields >= 1 &&
        atoi(m.fields[0]) == ERR_UNKNOWN_RCP)
        PASS("error 2 (unknown recipient) returned");
    else
        FAIL("error 2", "expected ERR code 2");

    close(fd);
}

static void t08_msg_too_long(const char *host, const char *port) {
    printf("T08: MSG body too long\n");
    int fd = connect_to(host, port);
    if (fd < 0) { FAIL("connect", "could not connect"); return; }
    do_login(fd, "Alice", NULL);
    drain(fd);

    /* 81 chars – one over the 80-char limit */
    char longmsg[82];
    memset(longmsg, 'x', 81);
    longmsg[81] = '\0';

    const char *fields[] = { "", "#all", longmsg, NULL };
    send_message(fd, CODE_MSG, fields);

    Message m;
    if (recv_message_timeout(fd, &m, 3) == 0 &&
        strcmp(m.code, CODE_ERR) == 0 &&
        m.nfields >= 1 &&
        atoi(m.fields[0]) == ERR_TOO_LONG)
        PASS("error 4 (too long) returned for long message body");
    else
        FAIL("error 4", "expected ERR code 4");

    close(fd);
}

static void t09_set_and_who(const char *host, const char *port) {
    printf("T09: SET status, WHO shows it\n");
    int alice = connect_to(host, port);
    int bob   = connect_to(host, port);
    if (alice < 0 || bob < 0) {
        FAIL("connect", "could not connect"); return;
    }
    do_login(alice, "Alice", NULL);
    do_login(bob,   "Bob",   NULL);
    drain(alice); drain(bob);

    /* Alice sets status */
    const char *sfields[] = { "Just chilling", NULL };
    send_message(alice, CODE_SET, sfields);
    drain(alice); drain(bob);  /* consume status-change announcements */

    /* Bob queries Alice */
    const char *wfields[] = { "Alice", NULL };
    send_message(bob, CODE_WHO, wfields);

    Message m;
    if (recv_message_timeout(bob, &m, 3) == 0 &&
        strcmp(m.code, CODE_MSG) == 0 &&
        m.nfields >= 3 &&
        strstr(m.fields[2], "Alice") != NULL &&
        strstr(m.fields[2], "Just chilling") != NULL)
        PASS("WHO response contains name and status");
    else
        FAIL("WHO with status", "response missing name or status");

    close(alice); close(bob);
}

static void t10_who_all(const char *host, const char *port) {
    printf("T10: WHO #all lists connected users\n");
    int alice = connect_to(host, port);
    int bob   = connect_to(host, port);
    if (alice < 0 || bob < 0) {
        FAIL("connect", "could not connect"); return;
    }
    do_login(alice, "Alice", NULL);
    do_login(bob,   "Bob",   NULL);
    drain(alice); drain(bob);

    const char *fields[] = { "#all", NULL };
    send_message(alice, CODE_WHO, fields);

    Message m;
    if (recv_message_timeout(alice, &m, 3) == 0 &&
        strcmp(m.code, CODE_MSG) == 0 &&
        m.nfields >= 3 &&
        strstr(m.fields[2], "Alice") != NULL &&
        strstr(m.fields[2], "Bob") != NULL)
        PASS("WHO #all lists both users");
    else
        FAIL("WHO #all", "response did not contain both users");

    close(alice); close(bob);
}

static void t11_who_unknown(const char *host, const char *port) {
    printf("T11: WHO for unknown user\n");
    int fd = connect_to(host, port);
    if (fd < 0) { FAIL("connect", "could not connect"); return; }
    do_login(fd, "Alice", NULL);
    drain(fd);

    const char *fields[] = { "Ghost", NULL };
    send_message(fd, CODE_WHO, fields);

    Message m;
    if (recv_message_timeout(fd, &m, 3) == 0 &&
        strcmp(m.code, CODE_ERR) == 0 &&
        m.nfields >= 1 &&
        atoi(m.fields[0]) == ERR_UNKNOWN_RCP)
        PASS("error 2 (unknown recipient) returned for WHO");
    else
        FAIL("WHO unknown", "expected ERR code 2");

    close(fd);
}

static void t12_clear_status(const char *host, const char *port) {
    printf("T12: SET empty string clears status\n");
    int alice = connect_to(host, port);
    int bob   = connect_to(host, port);
    if (alice < 0 || bob < 0) {
        FAIL("connect", "could not connect"); return;
    }
    do_login(alice, "Alice", NULL);
    do_login(bob,   "Bob",   NULL);
    drain(alice); drain(bob);

    /* set then clear */
    const char *s1[] = { "I am here", NULL };
    send_message(alice, CODE_SET, s1);
    drain(alice); drain(bob);

    const char *s2[] = { "", NULL };
    send_message(alice, CODE_SET, s2);
    drain(alice); drain(bob);

    /* WHO should now say "No status" */
    const char *wfields[] = { "Alice", NULL };
    send_message(bob, CODE_WHO, wfields);

    Message m;
    if (recv_message_timeout(bob, &m, 3) == 0 &&
        strcmp(m.code, CODE_MSG) == 0 &&
        m.nfields >= 3 &&
        strstr(m.fields[2], "No status") != NULL)
        PASS("WHO shows 'No status' after empty SET");
    else
        FAIL("clear status", "expected 'No status' in WHO response");

    close(alice); close(bob);
}

static void t13_disconnect_removes_user(const char *host, const char *port) {
    printf("T13: Disconnect removes user from WHO #all\n");
    int alice = connect_to(host, port);
    int bob   = connect_to(host, port);
    if (alice < 0 || bob < 0) {
        FAIL("connect", "could not connect"); return;
    }
    do_login(alice, "Alice", NULL);
    do_login(bob,   "Bob",   NULL);
    drain(alice); drain(bob);

    /* Alice disconnects */
    close(alice);
    sleep(1);  /* give server time to process the disconnect */

    const char *fields[] = { "#all", NULL };
    send_message(bob, CODE_WHO, fields);

    Message m;
    if (recv_message_timeout(bob, &m, 3) == 0 &&
        strcmp(m.code, CODE_MSG) == 0 &&
        m.nfields >= 3 &&
        strstr(m.fields[2], "Alice") == NULL &&
        strstr(m.fields[2], "Bob") != NULL)
        PASS("disconnected user removed from WHO #all");
    else
        FAIL("disconnect cleanup", "Alice still appears in WHO #all");

    close(bob);
}

/* ── main ── */

int main(int argc, char *argv[]) {
    if (argc != 3) {
        fprintf(stderr, "usage: %s <hostname> <port>\n", argv[0]);
        return 1;
    }
    const char *host = argv[1];
    const char *port = argv[2];

    printf("=== CS214 P4 Test Suite ===\n");
    printf("Server: %s:%s\n\n", host, port);

    t01_normal_login(host, port);
    t02_duplicate_name(host, port);
    t03_name_too_long(host, port);
    t04_illegal_char_in_name(host, port);
    t05_broadcast_msg(host, port);
    t06_private_msg(host, port);
    t07_unknown_recipient(host, port);
    t08_msg_too_long(host, port);
    t09_set_and_who(host, port);
    t10_who_all(host, port);
    t11_who_unknown(host, port);
    t12_clear_status(host, port);
    t13_disconnect_removes_user(host, port);

    printf("\n=== Results: %d passed, %d failed ===\n",
           pass_count, fail_count);
    return fail_count > 0 ? 1 : 0;
}

