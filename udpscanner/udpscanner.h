#pragma once

#ifdef WIN32
/* Windows */

#define WIN32_LEAN_AND_MEAN
#include <Windows.h>

#include <WinSock2.h>
#include <Ws2tcpip.h>

#pragma comment(lib, "ws2_32.lib")

#define LastSocketError() WSAGetLastError()

#define ERR_IS_REJECTION(x) ((x) == WSAECONNRESET)
#define ERR_IS_TIMEOUT(x) ((x) == WSAETIMEDOUT)
#define ERR_IS_TRUNCATION(x) ((x) == WSAEMSGSIZE)

#else
/* POSIX */

#include <unistd.h>

#include <sys/types.h>
#include <sys/socket.h>

#include <netinet/in.h>

#include <netdb.h>

typedef int SOCKET;
#define closesocket(x) close(x)

#include <errno.h>
#define SOCKET_ERROR -1
#define INVALID_SOCKET -1
#define LastSocketError() errno

#define ERR_IS_REJECTION(x) ((x) == ECONNREFUSED)
#define ERR_IS_TIMEOUT(x) ((x) == EWOULDBLOCK)
#define ERR_IS_TRUNCATION(x) ((x) == EMSGSIZE)

#endif

#define SCAN_RESULT_ERROR -1
#define SCAN_RESULT_PORT_CLOSED 0
#define SCAN_RESULT_PORT_INCONCLUSIVE 1
#define SCAN_RESULT_PORT_OPEN 2

#include <stdlib.h>
#include <stdio.h>
