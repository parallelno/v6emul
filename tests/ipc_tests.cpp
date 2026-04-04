#include <iostream>
#include <vector>
#include <memory>
#include <thread>
#include <chrono>
#include <cstring>

#include "core/hardware.h"
#include "core/display.h"
#include "core/fdd_consts.h"
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

// Simple TCP client for testing (connects to the server, sends/receives messages)
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

		// Receive response
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

// Helper: start server on a thread, return the port
struct ServerContext {
	dev::ipc::Transport server;
	std::unique_ptr<dev::Hardware> hw;
	uint16_t port = 0;
	std::thread acceptThread;
	std::thread loopThread;

	ServerContext() : hw(std::make_unique<dev::Hardware>("", "", true)) {}

	bool Start() {
		if (!server.Listen(0)) return false; // port 0 = OS picks
		port = server.GetPort();
		// Accept on background thread so test client can connect
		acceptThread = std::thread([this]() {
			server.AcceptClient();
		});
		return true;
	}

	void WaitForClient() {
		if (acceptThread.joinable()) acceptThread.join();
	}

	// Start the server message loop on a background thread
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

// ── Test: Ping/Pong ─────────────────────────────────────────────────
static void test_ping_pong()
{
	ServerContext ctx;
	ASSERT_TRUE(ctx.Start());

	TestClient client;
	ASSERT_TRUE(client.Connect(ctx.port));
	ctx.WaitForClient();
	ctx.StartLoop();

	// Send ping
	nlohmann::json pingReq = {
		{dev::ipc::FIELD_CMD, dev::ipc::CMD_PING},
		{dev::ipc::FIELD_DATA, {}}
	};
	auto resp = client.SendRequest(pingReq);
	ASSERT_TRUE(resp[dev::ipc::FIELD_OK].get<bool>());
	ASSERT_TRUE(resp[dev::ipc::FIELD_DATA]["pong"].get<bool>());

	client.CloseSocket();
	ctx.Close();
}

// ── Test: Hardware command round-trip (GET_REGS) ────────────────────
static void test_get_regs()
{
	ServerContext ctx;
	ASSERT_TRUE(ctx.Start());

	TestClient client;
	ASSERT_TRUE(client.Connect(ctx.port));
	ctx.WaitForClient();
	ctx.StartLoop();

	// Send GET_REGS
	nlohmann::json getRegsReq = {
		{dev::ipc::FIELD_CMD, static_cast<int>(dev::Hardware::Req::GET_REGS)},
		{dev::ipc::FIELD_DATA, {}}
	};
	auto resp = client.SendRequest(getRegsReq);
	ASSERT_TRUE(resp[dev::ipc::FIELD_OK].get<bool>());
	auto data = resp[dev::ipc::FIELD_DATA];
	// Registers should contain at least af, bc, de, hl, sp, pc keys
	ASSERT_TRUE(data.contains("af"));
	ASSERT_TRUE(data.contains("bc"));
	ASSERT_TRUE(data.contains("de"));
	ASSERT_TRUE(data.contains("hl"));
	ASSERT_TRUE(data.contains("sp"));
	ASSERT_TRUE(data.contains("pc"));

	client.CloseSocket();
	ctx.Close();
}

// ── Test: Memory write/read round-trip ──────────────────────────────
static void test_memory_round_trip()
{
	ServerContext ctx;
	ASSERT_TRUE(ctx.Start());

	TestClient client;
	ASSERT_TRUE(client.Connect(ctx.port));
	ctx.WaitForClient();
	ctx.StartLoop();

	// Write bytes to address 0x1000
	std::vector<uint8_t> testData = {0xAA, 0xBB, 0xCC, 0xDD};
	nlohmann::json setMemReq = {
		{dev::ipc::FIELD_CMD, static_cast<int>(dev::Hardware::Req::SET_MEM)},
		{dev::ipc::FIELD_DATA, {{"addr", 0x1000}, {"data", testData}}}
	};
	auto setResp = client.SendRequest(setMemReq);
	ASSERT_TRUE(setResp[dev::ipc::FIELD_OK].get<bool>());

	// Read back byte at 0x1000 (GET_BYTE_RAM)
	nlohmann::json getByteReq = {
		{dev::ipc::FIELD_CMD, static_cast<int>(dev::Hardware::Req::GET_BYTE_RAM)},
		{dev::ipc::FIELD_DATA, {{"addr", 0x1000}}}
	};
	auto getResp = client.SendRequest(getByteReq);
	ASSERT_TRUE(getResp[dev::ipc::FIELD_OK].get<bool>());
	auto readByte = getResp[dev::ipc::FIELD_DATA]["data"].get<uint8_t>();
	ASSERT_EQ(readByte, (uint8_t)0xAA);

	client.CloseSocket();
	ctx.Close();
}

// ── Test: Run ROM and fetch frame ───────────────────────────────────
static void test_run_and_fetch_frame()
{
	ServerContext ctx;
	ASSERT_TRUE(ctx.Start());

	TestClient client;
	ASSERT_TRUE(client.Connect(ctx.port));
	ctx.WaitForClient();
	ctx.StartLoop();

	// Load a small program: NOP; JMP 0
	std::vector<uint8_t> rom = {0x00, 0xC3, 0x00, 0x00};
	nlohmann::json setMemReq = {
		{dev::ipc::FIELD_CMD, static_cast<int>(dev::Hardware::Req::SET_MEM)},
		{dev::ipc::FIELD_DATA, {{"addr", 0}, {"data", rom}}}
	};
	client.SendRequest(setMemReq);

	// Restart
	nlohmann::json restartReq = {
		{dev::ipc::FIELD_CMD, static_cast<int>(dev::Hardware::Req::RESTART)},
		{dev::ipc::FIELD_DATA, {}}
	};
	client.SendRequest(restartReq);

	// Run headless for 1 frame
	nlohmann::json headlessReq = {
		{dev::ipc::FIELD_CMD, static_cast<int>(dev::Hardware::Req::RUN_HEADLESS)},
		{dev::ipc::FIELD_DATA, {{"haltExit", false}, {"maxFrames", 1}, {"maxCycles", 0}}}
	};
	auto runResp = client.SendRequest(headlessReq);
	ASSERT_TRUE(runResp[dev::ipc::FIELD_OK].get<bool>());
	auto data = runResp[dev::ipc::FIELD_DATA];
	ASSERT_EQ(data["frames"].get<uint64_t>(), (uint64_t)1);
	ASSERT_TRUE(data["cc"].get<uint64_t>() > 0);

	// Fetch display data
	nlohmann::json getDisplayReq = {
		{dev::ipc::FIELD_CMD, static_cast<int>(dev::Hardware::Req::GET_DISPLAY_DATA)},
		{dev::ipc::FIELD_DATA, {}}
	};
	auto displayResp = client.SendRequest(getDisplayReq);
	ASSERT_TRUE(displayResp[dev::ipc::FIELD_OK].get<bool>());

	client.CloseSocket();
	ctx.Close();
}

// ── Test: Protocol encode/decode round-trip ─────────────────────────
static void test_protocol_encode_decode()
{
	nlohmann::json original = {
		{"cmd", 42},
		{"data", {{"key", "value"}, {"num", 123}, {"arr", {1, 2, 3}}}}
	};

	auto encoded = dev::ipc::Encode(original);
	// First 4 bytes are length
	uint32_t len = 0;
	std::memcpy(&len, encoded.data(), 4);
	ASSERT_EQ(len, (uint32_t)(encoded.size() - 4));

	// Decode payload (skip length prefix)
	std::vector<uint8_t> payload(encoded.begin() + 4, encoded.end());
	auto decoded = dev::ipc::Decode(payload);
	ASSERT_EQ(decoded["cmd"].get<int>(), 42);
	ASSERT_EQ(decoded["data"]["key"].get<std::string>(), std::string("value"));
	ASSERT_EQ(decoded["data"]["num"].get<int>(), 123);
	ASSERT_EQ(decoded["data"]["arr"].size(), (size_t)3);
}

// ── Test: MakeResponse / MakeErrorResponse ──────────────────────────
static void test_response_helpers()
{
	auto okResp = dev::ipc::MakeResponse({{"result", 42}});
	ASSERT_TRUE(okResp[dev::ipc::FIELD_OK].get<bool>());
	ASSERT_EQ(okResp[dev::ipc::FIELD_DATA]["result"].get<int>(), 42);

	auto errResp = dev::ipc::MakeErrorResponse("something broke");
	ASSERT_TRUE(!errResp[dev::ipc::FIELD_OK].get<bool>());
	ASSERT_EQ(errResp[dev::ipc::FIELD_ERROR].get<std::string>(), std::string("something broke"));
}

// ── Test: LOAD_ROM via IPC ──────────────────────────────────────────
static void test_load_rom()
{
	ServerContext ctx;
	ASSERT_TRUE(ctx.Start());

	TestClient client;
	ASSERT_TRUE(client.Connect(ctx.port));
	ctx.WaitForClient();
	ctx.StartLoop();

	// Load a small program at address 0x100: MVI A, 0x42; HLT
	std::vector<uint8_t> rom = {0x3E, 0x42, 0x76};
	nlohmann::json loadRomReq = {
		{dev::ipc::FIELD_CMD, static_cast<int>(dev::Hardware::Req::LOAD_ROM)},
		{dev::ipc::FIELD_DATA, {{"data", rom}, {"addr", 0x100}, {"autorun", false}}}
	};
	auto resp = client.SendRequest(loadRomReq);
	ASSERT_TRUE(resp[dev::ipc::FIELD_OK].get<bool>());

	// Verify the data landed in RAM at 0x100
	nlohmann::json getByteReq = {
		{dev::ipc::FIELD_CMD, static_cast<int>(dev::Hardware::Req::GET_BYTE_RAM)},
		{dev::ipc::FIELD_DATA, {{"addr", 0x100}}}
	};
	auto getResp = client.SendRequest(getByteReq);
	ASSERT_TRUE(getResp[dev::ipc::FIELD_OK].get<bool>());
	ASSERT_EQ(getResp[dev::ipc::FIELD_DATA]["data"].get<uint8_t>(), (uint8_t)0x3E);

	// Verify CPU was restarted (PC should be 0 after Restart)
	nlohmann::json getPcReq = {
		{dev::ipc::FIELD_CMD, static_cast<int>(dev::Hardware::Req::GET_REG_PC)},
		{dev::ipc::FIELD_DATA, {}}
	};
	auto pcResp = client.SendRequest(getPcReq);
	ASSERT_TRUE(pcResp[dev::ipc::FIELD_OK].get<bool>());
	ASSERT_EQ(pcResp[dev::ipc::FIELD_DATA]["pc"].get<uint16_t>(), (uint16_t)0);

	client.CloseSocket();
	ctx.Close();
}

// ── Test: MOUNT_FDD via IPC ─────────────────────────────────────────
static void test_mount_fdd()
{
	ServerContext ctx;
	ASSERT_TRUE(ctx.Start());

	TestClient client;
	ASSERT_TRUE(client.Connect(ctx.port));
	ctx.WaitForClient();
	ctx.StartLoop();

	// Create a small FDD image (will be normalized to FDD_SIZE)
	std::vector<uint8_t> fddData(1024, 0xAA);
	nlohmann::json mountReq = {
		{dev::ipc::FIELD_CMD, static_cast<int>(dev::Hardware::Req::MOUNT_FDD)},
		{dev::ipc::FIELD_DATA, {{"data", fddData}, {"driveIdx", 1}, {"path", "test.fdd"}, {"autoBoot", false}}}
	};
	auto resp = client.SendRequest(mountReq);
	ASSERT_TRUE(resp[dev::ipc::FIELD_OK].get<bool>());

	// Verify drive 1 is mounted
	nlohmann::json infoReq = {
		{dev::ipc::FIELD_CMD, static_cast<int>(dev::Hardware::Req::GET_FDD_INFO)},
		{dev::ipc::FIELD_DATA, {{"driveIdx", 1}}}
	};
	auto infoResp = client.SendRequest(infoReq);
	ASSERT_TRUE(infoResp[dev::ipc::FIELD_OK].get<bool>());
	ASSERT_TRUE(infoResp[dev::ipc::FIELD_DATA]["mounted"].get<bool>());
	ASSERT_EQ(infoResp[dev::ipc::FIELD_DATA]["path"].get<std::string>(), std::string("test.fdd"));

	// Verify drive 0 is NOT mounted
	nlohmann::json info0Req = {
		{dev::ipc::FIELD_CMD, static_cast<int>(dev::Hardware::Req::GET_FDD_INFO)},
		{dev::ipc::FIELD_DATA, {{"driveIdx", 0}}}
	};
	auto info0Resp = client.SendRequest(info0Req);
	ASSERT_TRUE(info0Resp[dev::ipc::FIELD_OK].get<bool>());
	ASSERT_TRUE(!info0Resp[dev::ipc::FIELD_DATA]["mounted"].get<bool>());

	client.CloseSocket();
	ctx.Close();
}

// ── Test: FDD persistence workflow ──────────────────────────────────
static void test_fdd_persistence()
{
	ServerContext ctx;
	ASSERT_TRUE(ctx.Start());

	TestClient client;
	ASSERT_TRUE(client.Connect(ctx.port));
	ctx.WaitForClient();
	ctx.StartLoop();

	// Mount a FDD image on drive 0
	std::vector<uint8_t> fddData(FDD_SIZE, 0);
	fddData[0] = 0xDE;
	fddData[1] = 0xAD;
	nlohmann::json mountReq = {
		{dev::ipc::FIELD_CMD, static_cast<int>(dev::Hardware::Req::MOUNT_FDD)},
		{dev::ipc::FIELD_DATA, {{"data", fddData}, {"driveIdx", 0}, {"path", "persist.fdd"}, {"autoBoot", false}}}
	};
	auto mountResp = client.SendRequest(mountReq);
	ASSERT_TRUE(mountResp[dev::ipc::FIELD_OK].get<bool>());

	// Check initial state: mounted, not dirty
	nlohmann::json infoReq = {
		{dev::ipc::FIELD_CMD, static_cast<int>(dev::Hardware::Req::GET_FDD_INFO)},
		{dev::ipc::FIELD_DATA, {{"driveIdx", 0}}}
	};
	auto infoResp = client.SendRequest(infoReq);
	ASSERT_TRUE(infoResp[dev::ipc::FIELD_OK].get<bool>());
	ASSERT_TRUE(infoResp[dev::ipc::FIELD_DATA]["mounted"].get<bool>());
	ASSERT_TRUE(!infoResp[dev::ipc::FIELD_DATA]["updated"].get<bool>());

	// Export the disk image
	nlohmann::json getImgReq = {
		{dev::ipc::FIELD_CMD, static_cast<int>(dev::Hardware::Req::GET_FDD_IMAGE)},
		{dev::ipc::FIELD_DATA, {{"driveIdx", 0}}}
	};
	auto imgResp = client.SendRequest(getImgReq);
	ASSERT_TRUE(imgResp[dev::ipc::FIELD_OK].get<bool>());
	auto imgData = imgResp[dev::ipc::FIELD_DATA]["data"].get<std::vector<uint8_t>>();
	ASSERT_EQ(imgData.size(), (size_t)FDD_SIZE);
	ASSERT_EQ(imgData[0], (uint8_t)0xDE);
	ASSERT_EQ(imgData[1], (uint8_t)0xAD);

	// Clear dirty flag
	nlohmann::json resetReq = {
		{dev::ipc::FIELD_CMD, static_cast<int>(dev::Hardware::Req::RESET_UPDATE_FDD)},
		{dev::ipc::FIELD_DATA, {{"driveIdx", 0}}}
	};
	auto resetResp = client.SendRequest(resetReq);
	ASSERT_TRUE(resetResp[dev::ipc::FIELD_OK].get<bool>());

	client.CloseSocket();
	ctx.Close();
}

int main()
{
	test_protocol_encode_decode();
	test_response_helpers();
	test_ping_pong();
	test_get_regs();
	test_memory_round_trip();
	test_run_and_fetch_frame();
	test_load_rom();
	test_mount_fdd();
	test_fdd_persistence();

	std::cout << "IPC Tests: " << tests_passed << "/" << tests_run << " passed";
	if (tests_failed > 0) {
		std::cout << " (" << tests_failed << " FAILED)";
	}
	std::cout << std::endl;
	return (tests_failed == 0) ? 0 : 1;
}
