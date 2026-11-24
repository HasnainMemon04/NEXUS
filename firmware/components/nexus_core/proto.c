/*
 * proto.c — framing over a TCP stream: read and write whole messages.
 */
#include <errno.h>
#include "lwip/sockets.h"
#include "proto.h"

static bool recv_all(int sock, void *data, size_t len, bool allow_initial_eof)
{
    uint8_t *p = data;
    while (len > 0) {
        int n = recv(sock, p, len, 0);
        if (n == 0) {
            errno = allow_initial_eof && p == data ? 0 : EBADMSG;
            return false;
        }
        if (n < 0) {
            if (errno == EINTR) continue;
            return false;
        }
        p += n;
        len -= (size_t)n;
    }
    return true;
}

bool nx_send_frame(int sock, uint8_t type, const void *a, uint16_t alen, const void *b, uint16_t blen)
{
    if ((uint32_t)alen + blen > NX_MAX_FRAME) {
        errno = EMSGSIZE;
        return false;
    }
    if ((alen && !a) || (blen && !b)) {
        errno = EINVAL;
        return false;
    }

    nx_frame_hdr_t header = { .magic = NX_MAGIC, .version = NX_VERSION, .type = type, .len = alen + blen };
    struct iovec parts[3] = { { .iov_base = &header, .iov_len = sizeof(header) } };
    struct msghdr message = { .msg_iov = parts, .msg_iovlen = 1 };
    if (alen) parts[message.msg_iovlen++] = (struct iovec){ .iov_base = (void *)a, .iov_len = alen };
    if (blen) parts[message.msg_iovlen++] = (struct iovec){ .iov_base = (void *)b, .iov_len = blen };

    /* One lwIP write per complete frame in the normal case. Avoid small TCP
     * writes for the header and heartbeat trailer on the Wi-Fi link. */
    while (message.msg_iovlen) {
        int n = lwip_sendmsg(sock, &message, 0);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) {
            if (n == 0) errno = EPIPE;
            return false;
        }
        size_t sent = (size_t)n;
        while (message.msg_iovlen && sent >= message.msg_iov->iov_len) {
            sent -= message.msg_iov->iov_len;
            message.msg_iov++;
            message.msg_iovlen--;
        }
        if (sent) {
            message.msg_iov->iov_base = (uint8_t *)message.msg_iov->iov_base + sent;
            message.msg_iov->iov_len -= sent;
        }
    }
    return true;
}

int nx_recv_frame(int sock, uint8_t *buf, uint16_t cap, uint8_t *type)
{
    nx_frame_hdr_t header;
    if (!recv_all(sock, &header, sizeof(header), true)) return -1;
    if (header.magic != NX_MAGIC || header.version != NX_VERSION ||
        header.len > cap || header.len > NX_MAX_FRAME || !type || (header.len && !buf)) {
        errno = EBADMSG;
        return -1;
    }
    if (header.len && !recv_all(sock, buf, header.len, false)) return -1;
    *type = header.type;
    return header.len;
}
