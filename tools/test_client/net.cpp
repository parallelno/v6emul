#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <WinSock2.h>
#include <WS2tcpip.h>
#include <Windows.h>

#include "net.h"

SOCKET            g_sock      = INVALID_SOCKET;
uint16_t          g_port      = 9876;
std::atomic<bool> g_connected{false};

bool RecvExact(void* buf, size_t len)
{
	auto* p = static_cast<char*>(buf);
	size_t remaining = len;
	while (remaining > 0) {
		int n = recv(g_sock, p, static_cast<int>(remaining), 0);
		if (n <= 0) return false;
		p += n;
		remaining -= n;
	}
	return true;
}

bool SendExact(const void* buf, size_t len)
{
	auto* p = static_cast<const char*>(buf);
	size_t remaining = len;
	while (remaining > 0) {
		int n = send(g_sock, p, static_cast<int>(remaining), 0);
		if (n <= 0) return false;
		p += n;
		remaining -= n;
	}
	return true;
}

bool ConnectToServer()
{
	if (g_sock != INVALID_SOCKET) {
		closesocket(g_sock);
		g_sock = INVALID_SOCKET;
	}

	g_sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
	if (g_sock == INVALID_SOCKET) return false;

	BOOL nodelay = TRUE;
	setsockopt(g_sock, IPPROTO_TCP, TCP_NODELAY,
		reinterpret_cast<const char*>(&nodelay), sizeof(nodelay));

	int rcvBuf = 4 * 1024 * 1024;
	setsockopt(g_sock, SOL_SOCKET, SO_RCVBUF,
		reinterpret_cast<const char*>(&rcvBuf), sizeof(rcvBuf));

	sockaddr_in addr{};
	addr.sin_family = AF_INET;
	addr.sin_port   = htons(g_port);
	inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);

	if (connect(g_sock, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == SOCKET_ERROR) {
		closesocket(g_sock);
		g_sock = INVALID_SOCKET;
		return false;
	}

	g_connected.store(true);
	return true;
}

void Disconnect()
{
	if (g_sock != INVALID_SOCKET) {
		closesocket(g_sock);
		g_sock = INVALID_SOCKET;
	}
	g_connected.store(false);
}
