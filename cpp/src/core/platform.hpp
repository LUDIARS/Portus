#pragma once
//OS ごとのソケットヘッダと poll の差を吸収する (src 内部専用)
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
namespace portus {
using pollfd_t = WSAPOLLFD;
inline int poll_sockets(pollfd_t* fds, unsigned long count, int timeout_ms) { return WSAPoll(fds, count, timeout_ms); }
}
#else
#include <arpa/inet.h>
#include <cerrno>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>
namespace portus {
using pollfd_t = struct pollfd;
inline int poll_sockets(pollfd_t* fds, unsigned long count, int timeout_ms) { return ::poll(fds, count, timeout_ms); }
}
#endif
