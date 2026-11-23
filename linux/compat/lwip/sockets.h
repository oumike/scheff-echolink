#pragma once

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

// esp_restart() execs this process; none of its network sockets may survive.
static inline int el_socket(int domain, int type, int protocol)
{
    return socket(domain, type | SOCK_CLOEXEC, protocol);
}

#define socket el_socket
