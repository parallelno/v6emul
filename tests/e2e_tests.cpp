// End-to-end test: client → TCP → run ROM → fetch frame → verify pixels.
//
// Exercises the full IPC path using an in-process server with the real
// Hardware engine, TCP transport, and a test client — exactly the same
// data path as when v6emul runs with --serve.

#include <iostream>
#include <vector>
#include <memory>
#include <thread>
#include <chrono>
#include <cstring>
#include <numeric>

#include "core/hardware.h"
#include "core/display.h"
#include "ipc/transport.h"
#include "ipc/protocol.h"
#include "ipc/commands.h"

static int tests_run = 0;
static int tests_passed = 0;
static int tests_failed = 0;

#define ASSERT_EQ(a, b) do { \
    tests_run++; \
    if ((a) == (b)) { tests_passed++; } \
    else { \
        tests_failed++; \
        std::cerr << "FAIL: " << #a << " == " << #b \
                  << " (got " << (a) << " != " << (b) << ")" \
                  << " at " << __FILE__ << ":" << __LINE__ << std::endl; \
    } \
} while(0)

#define ASSERT_TRUE(cond) do { \
    tests_run++; \
    if (cond) { tests_passed++; } \
    else { \
        tests_failed++; \
        std::cerr << "FAIL: " << #cond \
                  << " at " << __FILE__ << ":" << __LINE__ << std::endl; \
    } \
} while(0)

// ── TCP test client (same as ipc_tests) ─────────────────────────────
class TestClient
{
public:
	bool Connect(uint16_t _port)
	{
#ifdef _WIN32
		static bool wsaInit = false;
		if (!wsaInit) {
			WSADATA wsaData;
			WSAStartup(MAKEWORD(2, 2), &wsaData);
			wsaInit = true;
		}
#endif
		m_sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
		if (m_sock == INVALID_SOCK) return false;

		sockaddr_in addr{};
		addr.sin_family = AF_INET;
		addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
		addr.sin_port = htons(_port);

		if (connect(m_sock, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
			CloseSocket();
			return false;
		}

		int opt = 1;
		setsockopt(m_sock, IPPROTO_TCP, TCP_NODELAY,
			reinterpret_cast<const char*>(&opt), sizeof(opt));

		return true;
	}

	nlohmann::json SendRequest(const nlohmann::json& _request)
	{
		auto encoded = dev::ipc::Encode(_request);
		SendExact(encoded.data(), encoded.size());

		uint32_t len = 0;
		RecvExact(&len, 4);
		std::vector<uint8_t> payload(len);
		RecvExact(payload.data(), len);
		return dev::ipc::Decode(payload);
	}

	void CloseSocket()
	{
		if (m_sock != INVALID_SOCK) {
#ifdef _WIN32
			closesocket(m_sock);
#else
			close(m_sock);
#endif
			m_sock = INVALID_SOCK;
		}
	}

	~TestClient() { CloseSocket(); }

private:
#ifdef _WIN32
	using SocketType = SOCKET;
	static constexpr SocketType INVALID_SOCK = INVALID_SOCKET;
#else
	using SocketType = int;
	static constexpr SocketType INVALID_SOCK = -1;
#endif

	SocketType m_sock = INVALID_SOCK;

	void RecvExact(void* buf, size_t len) {
		auto* p = static_cast<char*>(buf);
		size_t rem = len;
		while (rem > 0) {
			int n = recv(m_sock, p, static_cast<int>(rem), 0);
			if (n <= 0) break;
			p += n;
			rem -= n;
		}
	}

	void SendExact(const void* buf, size_t len) {
		auto* p = static_cast<const char*>(buf);
		size_t rem = len;
		while (rem > 0) {
			int n = send(m_sock, p, static_cast<int>(rem), 0);
			if (n <= 0) break;
			p += n;
			rem -= n;
		}
	}
};

// ── Server context (mirrors ipc_tests pattern) ──────────────────────
struct ServerContext {
	dev::ipc::Transport server;
	std::unique_ptr<dev::Hardware> hw;
	uint16_t port = 0;
	std::thread acceptThread;
	std::thread loopThread;

	ServerContext() : hw(std::make_unique<dev::Hardware>("", "", true)) {}

	bool Start() {
		if (!server.Listen(0)) return false;
		port = server.GetPort();
		acceptThread = std::thread([this]() {
			server.AcceptClient();
		});
		return true;
	}

	void WaitForClient() {
		if (acceptThread.joinable()) acceptThread.join();
	}

	void StartLoop() {
		loopThread = std::thread([this]() {
			while (server.IsClientConnected()) {
				auto payload = server.Recv();
				if (payload.empty()) break;

				try {
					auto requestJ = dev::ipc::Decode(payload);
					int cmdInt = requestJ.value(dev::ipc::FIELD_CMD, 0);
					auto dataJ = requestJ.value(dev::ipc::FIELD_DATA, nlohmann::json{});

					if (cmdInt == dev::ipc::CMD_PING) {
						auto resp = dev::ipc::Encode(dev::ipc::MakeResponse({{"pong", true}}));
						server.Send(resp);
						continue;
					}

					if (cmdInt == dev::ipc::CMD_GET_FRAME) {
						auto* fb = hw->GetFrame(false);
						nlohmann::json responseJ;
						if (fb) {
							auto* raw = reinterpret_cast<const uint8_t*>(fb->data());
							size_t len = fb->size() * sizeof(dev::ColorI);
							responseJ = dev::ipc::MakeResponse({
								{"width", dev::Display::FRAME_W},
								{"height", dev::Display::FRAME_H},
								{"pixels", nlohmann::json::binary_t({raw, raw + len})}
							});
						} else {
							responseJ = dev::ipc::MakeErrorResponse("no frame available");
						}
						if (!server.Send(dev::ipc::Encode(responseJ))) break;
						continue;
					}

					auto req = static_cast<dev::Hardware::Req>(cmdInt);
					auto result = hw->Request(req, dataJ);

					nlohmann::json responseJ;
					if (result) {
						responseJ = dev::ipc::MakeResponse(*result);
					} else {
						responseJ = dev::ipc::MakeErrorResponse("request failed");
					}
					if (!server.Send(dev::ipc::Encode(responseJ))) break;
				} catch (const std::exception& e) {
					auto errResp = dev::ipc::Encode(
						dev::ipc::MakeErrorResponse(std::string("server error: ") + e.what()));
					if (!server.Send(errResp)) break;
				}
			}
		});
	}

	void Close() {
		server.Close();
		if (acceptThread.joinable()) acceptThread.join();
		if (loopThread.joinable()) loopThread.join();
	}
};

// ── End-to-end test ─────────────────────────────────────────────────
// Full flow: connect → load ROM → restart → run headless (1 frame) →
//            fetch frame buffer → verify pixel data is valid ABGR.
static void test_e2e_load_run_fetch_frame()
{
	ServerContext ctx;
	ASSERT_TRUE(ctx.Start());

	TestClient client;
	ASSERT_TRUE(client.Connect(ctx.port));
	ctx.WaitForClient();
	ctx.StartLoop();

	// 1. Ping to verify connectivity
	{
		nlohmann::json req = {
			{dev::ipc::FIELD_CMD, dev::ipc::CMD_PING},
			{dev::ipc::FIELD_DATA, {}}
		};
		auto resp = client.SendRequest(req);
		ASSERT_TRUE(resp[dev::ipc::FIELD_OK].get<bool>());
	}

	// 2. Load a small ROM: NOP; NOP; JMP 0
	//    This runs a tight loop, producing at least one rasterized frame.
	{
		std::vector<uint8_t> rom = {0x00, 0x00, 0xC3, 0x00, 0x00};
		nlohmann::json req = {
			{dev::ipc::FIELD_CMD, static_cast<int>(dev::Hardware::Req::SET_MEM)},
			{dev::ipc::FIELD_DATA, {{"addr", 0}, {"data", rom}}}
		};
		auto resp = client.SendRequest(req);
		ASSERT_TRUE(resp[dev::ipc::FIELD_OK].get<bool>());
	}

	// 3. Restart (disable ROM mapping, reset CPU to PC=0)
	{
		nlohmann::json req = {
			{dev::ipc::FIELD_CMD, static_cast<int>(dev::Hardware::Req::RESTART)},
			{dev::ipc::FIELD_DATA, {}}
		};
		auto resp = client.SendRequest(req);
		ASSERT_TRUE(resp[dev::ipc::FIELD_OK].get<bool>());
	}

	// 4. Run headless for 1 frame
	{
		nlohmann::json req = {
			{dev::ipc::FIELD_CMD, static_cast<int>(dev::Hardware::Req::RUN_HEADLESS)},
			{dev::ipc::FIELD_DATA, {{"haltExit", false}, {"maxFrames", 1}, {"maxCycles", 0}}}
		};
		auto resp = client.SendRequest(req);
		ASSERT_TRUE(resp[dev::ipc::FIELD_OK].get<bool>());
		auto data = resp[dev::ipc::FIELD_DATA];
		ASSERT_EQ(data["frames"].get<uint64_t>(), (uint64_t)1);
		ASSERT_TRUE(data["cc"].get<uint64_t>() > 0);
	}

	// 5. Fetch the frame buffer via CMD_GET_FRAME
	{
		nlohmann::json req = {
			{dev::ipc::FIELD_CMD, dev::ipc::CMD_GET_FRAME},
			{dev::ipc::FIELD_DATA, {}}
		};
		auto resp = client.SendRequest(req);
		ASSERT_TRUE(resp[dev::ipc::FIELD_OK].get<bool>());

		auto data = resp[dev::ipc::FIELD_DATA];
		ASSERT_EQ(data["width"].get<int>(), dev::Display::FRAME_W);
		ASSERT_EQ(data["height"].get<int>(), dev::Display::FRAME_H);

		// Pixels arrive as binary_t (raw ABGR bytes)
		auto pixels = data["pixels"].get<nlohmann::json::binary_t>();
		size_t expectedSize = static_cast<size_t>(dev::Display::FRAME_W)
							  * dev::Display::FRAME_H * sizeof(dev::ColorI);
		ASSERT_EQ(pixels.size(), expectedSize);

		// Verify the frame is not all zeros — at least the border region
		// should contain non-zero ABGR pixels (alpha = 0xFF for opaque).
		uint64_t sum = 0;
		auto* px = reinterpret_cast<const uint32_t*>(pixels.data());
		size_t pixelCount = pixels.size() / sizeof(uint32_t);
		for (size_t i = 0; i < pixelCount; ++i) {
			sum += px[i];
		}
		ASSERT_TRUE(sum > 0);

		// Verify at least some pixels have the alpha channel set (0xFF000000)
		size_t opaqueCount = 0;
		for (size_t i = 0; i < pixelCount; ++i) {
			if ((px[i] & 0xFF000000) == 0xFF000000) {
				opaqueCount++;
			}
		}
		ASSERT_TRUE(opaqueCount > 0);
	}

	// 6. Verify registers are accessible after the run
	{
		nlohmann::json req = {
			{dev::ipc::FIELD_CMD, static_cast<int>(dev::Hardware::Req::GET_REGS)},
			{dev::ipc::FIELD_DATA, {}}
		};
		auto resp = client.SendRequest(req);
		ASSERT_TRUE(resp[dev::ipc::FIELD_OK].get<bool>());
		ASSERT_TRUE(resp[dev::ipc::FIELD_DATA].contains("pc"));
	}

	client.CloseSocket();
	ctx.Close();
}

// ── Main ─────────────────────────────────────────────────────────────
int main()
{
	test_e2e_load_run_fetch_frame();

	std::cout << "E2E Tests: " << tests_passed << "/" << tests_run << " passed";
	if (tests_failed > 0) {
		std::cout << " (" << tests_failed << " FAILED)";
	}
	std::cout << std::endl;
	return (tests_failed == 0) ? 0 : 1;
}
