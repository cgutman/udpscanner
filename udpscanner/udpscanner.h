#pragma once

#ifdef WIN32
#define WIN32_LEAN_AND_MEAN
#include <Windows.h>

#include <WinSock2.h>
#include <Ws2tcpip.h>

#pragma comment(lib, "ws2_32.lib")

#define LAST_SOCKET_ERROR() WSAGetLastError()
#else

#endif

#define SCAN_RESULT_ERROR -1
#define SCAN_RESULT_PORT_CLOSED 0
#define SCAN_RESULT_PORT_INCONCLUSIVE 1
#define SCAN_RESULT_PORT_OPEN 2

#include <stdlib.h>
#include <stdio.h>