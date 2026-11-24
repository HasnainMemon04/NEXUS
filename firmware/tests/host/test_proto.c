/* Force fragmentation and interruptions in the production TCP framing. */
#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include "proto.h"
#include "lwip/sockets.h"

static uint8_t stream[NX_MAX_FRAME * 2];
static size_t length, cursor;
static unsigned tx_fragment = 5, rx_fragment = 2;
static bool send_interrupt, receive_interrupt, no_progress;

int lwip_sendmsg(int socket, const struct msghdr *message, int flags)
{
    (void)socket;
    (void)flags;
    if (no_progress) return 0;
    if (send_interrupt) { send_interrupt = false; errno = EINTR; return -1; }
    size_t written = 0;
    for (int i = 0; i < message->msg_iovlen && written < tx_fragment; i++) {
        const struct iovec *part = &message->msg_iov[i];
        size_t count = part->iov_len;
        if (count > tx_fragment - written) count = tx_fragment - written;
        assert(length + count <= sizeof(stream));
        memcpy(stream + length, part->iov_base, count);
        length += count;
        written += count;
    }
    return (int)written;
}

int recv(int socket, void *data, size_t len, int flags)
{
    (void)socket;
    (void)flags;
    if (receive_interrupt) { receive_interrupt = false; errno = EINTR; return -1; }
    if (len > rx_fragment) len = rx_fragment;
    if (len > length - cursor) len = length - cursor;
    memcpy(data, stream + cursor, len);
    cursor += len;
    return (int)len;
}

static void test_combined_frames(void)
{
    const uint8_t left[] = {1, 2, 3, 4};
    const uint8_t right[] = {5, 6, 7};
    const uint8_t expected[] = { 0x4e, 0x58, 1, NX_MSG_RESULT, 7, 0, 0, 0, 1, 2, 3, 4, 5, 6, 7 };
    send_interrupt = true;
    receive_interrupt = true;
    assert(nx_send_frame(0, NX_MSG_RESULT, left, sizeof(left), right, sizeof(right)));
    assert(length == sizeof(expected));
    assert(memcmp(stream, expected, sizeof(expected)) == 0);
    assert(nx_send_frame(0, NX_MSG_WELCOME, right, 1, NULL, 0));
    uint8_t buf[20], type = 0;
    assert(nx_recv_frame(0, buf, sizeof(buf), &type) == 7);
    assert(type == NX_MSG_RESULT && memcmp(buf, expected + sizeof(nx_frame_hdr_t), 7) == 0);
    assert(nx_recv_frame(0, buf, sizeof(buf), &type) == 1);
    assert(type == NX_MSG_WELCOME && buf[0] == right[0]);
    assert(nx_recv_frame(0, buf, sizeof(buf), &type) == -1 && errno == 0); /* orderly EOF */
}

static void test_invalid_frame(void)
{
    nx_frame_hdr_t hdr = { .magic = NX_MAGIC, .version = NX_VERSION, .type = NX_MSG_ACK, .len = 8 };
    memcpy(stream, &hdr, sizeof(hdr));
    uint8_t buf[16], type = 0;
    cursor = 0;
    length = sizeof(hdr) - 1;
    assert(nx_recv_frame(0, buf, sizeof(buf), &type) == -1 && errno == EBADMSG); /* truncated header */
    cursor = 0;
    length = sizeof(hdr) + 7;
    assert(nx_recv_frame(0, buf, sizeof(buf), &type) == -1 && errno == EBADMSG); /* truncated payload */
    cursor = 0;
    length = sizeof(hdr) + 8;
    assert(nx_recv_frame(0, buf, 7, &type) == -1 && errno == EBADMSG);           /* user buffer capacity */
    cursor = 0;
    stream[0] = 0;
    assert(nx_recv_frame(0, buf, sizeof(buf), &type) == -1 && errno == EBADMSG); /* bad magic */
    cursor = 0;
    stream[0] = 0x4e;
    stream[2] = 0xff;
    assert(nx_recv_frame(0, buf, sizeof(buf), &type) == -1 && errno == EBADMSG); /* incompatible version */
    cursor = 0;
    hdr.len = NX_MAX_FRAME + 1;
    memcpy(stream, &hdr, sizeof(hdr));
    assert(nx_recv_frame(0, buf, UINT16_MAX, &type) == -1 && errno == EBADMSG); /* protocol capacity */
    assert(!nx_send_frame(0, NX_MSG_ACK, buf, UINT16_MAX, buf, 1) && errno == EMSGSIZE); /* sum overflow */
    assert(!nx_send_frame(0, NX_MSG_ACK, NULL, 1, NULL, 0) && errno == EINVAL);
    no_progress = true;
    assert(!nx_send_frame(0, NX_MSG_ACK, NULL, 0, NULL, 0) && errno == EPIPE); /* zero-byte send */
}

int main(void)
{
    test_combined_frames();
    test_invalid_frame();
    puts("PASS NX v1 bytes, fragmented/combined TCP messages, EINTR and invalid/truncated frames");
    return 0;
}
