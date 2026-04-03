#pragma once

// TCP loopback transport: single-client server for IPC.
//
// Usage:
//   Transport server;
//   server.Listen(9876);         // bind + listen
//   server.AcceptClient();       // block until client connects
//   auto msg = server.Recv();    // receive one length-prefixed message
//   server.Send(responseBytes);  // send one length-prefixed message
//   server.Close();              // shut down

#include <vector>
#include <cstdint>
#include <string>
#include <functional>
#include <nlohmann/json.hpp>

#ifdef _WIN32
#include <WinSock2.h>
#include <WS2tcpip.h>
#pragma comment(lib, "ws2_32.lib")
using SocketType = SOCKET;
constexpr SocketType INVALID_SOCK = INVALID_SOCKET;
#else
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <unistd.h>
using SocketType = int;
constexpr SocketType INVALID_SOCK = -1;
#endif

namespace dev::ipc
{
	class Transport
	{
	public:
		Transport();
		~Transport();

		Transport(const Transport&) = delete;
		Transport& operator=(const Transport&) = delete;

		// Bind and listen on the given port. Returns true on success.
		bool Listen(uint16_t _port);

		// Block until a client connects. Returns true on success.
		bool AcceptClient();

		// Receive one length-prefixed MessagePack message from the client.
		// Returns the MessagePack payload (without the length prefix).
		// Returns empty vector on disconnect or error.
		auto Recv() -> std::vector<uint8_t>;

		// Send a length-prefixed MessagePack message to the client.
		// Returns true on success.
		bool Send(const std::vector<uint8_t>& _data);

		// Close all sockets.
		void Close();

		// True if a client is currently connected.
		bool IsClientConnected() const;

		// Get the port the server is actually listening on (useful if port 0 was passed).
		uint16_t GetPort() const;

	private:
		SocketType m_listenSock = INVALID_SOCK;
		SocketType m_clientSock = INVALID_SOCK;
		uint16_t m_port = 0;

		bool RecvExact(void* _buf, size_t _len);
		bool SendExact(const void* _buf, size_t _len);

		static void InitSockets();
		static void CleanupSockets();
		static void CloseSocket(SocketType& _sock);
	};

} // namespace dev::ipc
