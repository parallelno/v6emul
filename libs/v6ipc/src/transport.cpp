#include "ipc/transport.h"
#include "ipc/protocol.h"

#include <cstring>
#include <iostream>

#ifdef _WIN32
static bool g_wsaInitialized = false;
#endif

namespace dev::ipc
{

Transport::Transport()
{
	InitSockets();
}

Transport::~Transport()
{
	Close();
}

void Transport::InitSockets()
{
#ifdef _WIN32
	if (!g_wsaInitialized) {
		WSADATA wsaData;
		if (WSAStartup(MAKEWORD(2, 2), &wsaData) != 0) {
			std::cerr << "WSAStartup failed" << std::endl;
		}
		g_wsaInitialized = true;
	}
#endif
}

void Transport::CleanupSockets()
{
#ifdef _WIN32
	if (g_wsaInitialized) {
		WSACleanup();
		g_wsaInitialized = false;
	}
#endif
}

void Transport::CloseSocket(SocketType& _sock)
{
	if (_sock != INVALID_SOCK) {
#ifdef _WIN32
		closesocket(_sock);
#else
		close(_sock);
#endif
		_sock = INVALID_SOCK;
	}
}

bool Transport::Listen(uint16_t _port)
{
	m_listenSock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
	if (m_listenSock == INVALID_SOCK) return false;

	// Allow address reuse
	int opt = 1;
	setsockopt(m_listenSock, SOL_SOCKET, SO_REUSEADDR,
		reinterpret_cast<const char*>(&opt), sizeof(opt));

	sockaddr_in addr{};
	addr.sin_family = AF_INET;
	addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
	addr.sin_port = htons(_port);

	if (bind(m_listenSock, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
		CloseSocket(m_listenSock);
		return false;
	}

	if (listen(m_listenSock, 1) != 0) {
		CloseSocket(m_listenSock);
		return false;
	}

	// Query actual port (important when _port == 0)
	sockaddr_in boundAddr{};
	int addrLen = sizeof(boundAddr);
	if (getsockname(m_listenSock, reinterpret_cast<sockaddr*>(&boundAddr), &addrLen) == 0) {
		m_port = ntohs(boundAddr.sin_port);
	}

	return true;
}

bool Transport::AcceptClient()
{
	if (m_listenSock == INVALID_SOCK) return false;

	m_clientSock = accept(m_listenSock, nullptr, nullptr);
	if (m_clientSock == INVALID_SOCK) return false;

	// Disable Nagle for low-latency
	int opt = 1;
	setsockopt(m_clientSock, IPPROTO_TCP, TCP_NODELAY,
		reinterpret_cast<const char*>(&opt), sizeof(opt));

	return true;
}

bool Transport::RecvExact(void* _buf, size_t _len)
{
	auto* ptr = static_cast<char*>(_buf);
	size_t remaining = _len;
	while (remaining > 0) {
		int n = recv(m_clientSock, ptr, static_cast<int>(remaining), 0);
		if (n <= 0) return false;
		ptr += n;
		remaining -= n;
	}
	return true;
}

bool Transport::SendExact(const void* _buf, size_t _len)
{
	auto* ptr = static_cast<const char*>(_buf);
	size_t remaining = _len;
	while (remaining > 0) {
		int n = send(m_clientSock, ptr, static_cast<int>(remaining), 0);
		if (n <= 0) return false;
		ptr += n;
		remaining -= n;
	}
	return true;
}

auto Transport::Recv() -> std::vector<uint8_t>
{
	// Read 4-byte length prefix (little-endian)
	uint32_t len = 0;
	if (!RecvExact(&len, 4)) return {};

	// Sanity check: reject messages > 64 MB
	if (len > 64 * 1024 * 1024) return {};

	std::vector<uint8_t> payload(len);
	if (!RecvExact(payload.data(), len)) return {};

	return payload;
}

bool Transport::Send(const std::vector<uint8_t>& _data)
{
	// The data should already be length-prefixed (from Encode())
	return SendExact(_data.data(), _data.size());
}

void Transport::Close()
{
	CloseSocket(m_clientSock);
	CloseSocket(m_listenSock);
}

bool Transport::IsClientConnected() const
{
	return m_clientSock != INVALID_SOCK;
}

uint16_t Transport::GetPort() const
{
	return m_port;
}

} // namespace dev::ipc
