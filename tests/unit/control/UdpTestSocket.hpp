#pragma once

// The Winsock calls the UDP tests use, on Windows as they were and on macOS as BSD sockets
// (the same shim style as src/control/OscServer.cpp).

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <ws2tcpip.h>

using SocketLength = int;
// select()'s first argument is ignored by Winsock.
inline int selectRange(SOCKET) { return 0; }
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

using SOCKET = int;
using SocketLength = socklen_t;
struct WSADATA {};
#define MAKEWORD(a, b) 0
inline int WSAStartup(int, WSADATA*) { return 0; }
inline int WSACleanup() { return 0; }
inline int closesocket(SOCKET s) { return ::close(s); }
// BSD select() needs the highest descriptor plus one.
inline int selectRange(SOCKET s) { return s + 1; }
#endif
