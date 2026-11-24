#pragma once
#include <stddef.h>
struct iovec { void *iov_base; size_t iov_len; };
struct msghdr { struct iovec *msg_iov; int msg_iovlen; };
int lwip_sendmsg(int socket, const struct msghdr *message, int flags);
int recv(int socket, void *data, size_t length, int flags);
