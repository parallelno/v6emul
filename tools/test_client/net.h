#pragma once
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <WinSock2.h>
#include <WS2tcpip.h>
#include <Windows.h>

#include <atomic>
#include <cstdint>

extern SOCKET              g_sock;
extern uint16_t            g_port;
extern std::atomic<bool>   g_connected;

bool RecvExact(void* buf, size_t len);
bool SendExact(const void* buf, size_t len);
bool ConnectToServer();
void Disconnect();
