#include <iostream>
#include <vector>
#include <memory>
#include <thread>
#include <chrono>
#include <cstring>
#include <algorithm>

#include "core/hardware.h"
#include "core/debugger.h"
#include "core/display.h"
#include "core/fdd_consts.h"
#include "ipc/transport.h"
#include "ipc/protocol.h"
#include "ipc/raw_frame.h"
#include "ipc/server_info.h"
#include "ipc/commands.h"
#include "ipc_request.h"

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
						auto [pixels, region] = hw->GetFrame(false);
						nlohmann::json responseJ;
						if (pixels) {
							auto* raw = reinterpret_cast<const uint8_t*>(pixels);
							size_t len = static_cast<size_t>(region.width) * region.height * sizeof(dev::ColorI);
							responseJ = dev::ipc::MakeResponse({
								{"width", region.width},
								{"height", region.height},
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

static void test_malformed_request_does_not_stop_emulation()
{
	ServerContext ctx;
	ASSERT_TRUE(ctx.Start());

	TestClient client;
	ASSERT_TRUE(client.Connect(ctx.port));
	ctx.WaitForClient();
	ctx.StartLoop();

	nlohmann::json malformedRequest = {
		{dev::ipc::FIELD_CMD, static_cast<int>(dev::Hardware::Req::GET_STACK_SAMPLE)},
		{dev::ipc::FIELD_DATA, {}}
	};
	auto errorResponse = client.SendRequest(malformedRequest);
	ASSERT_TRUE(!errorResponse[dev::ipc::FIELD_OK].get<bool>());

	nlohmann::json validRequest = {
		{dev::ipc::FIELD_CMD, static_cast<int>(dev::Hardware::Req::GET_REGS)},
		{dev::ipc::FIELD_DATA, {}}
	};
	auto validResponse = client.SendRequest(validRequest);
	ASSERT_TRUE(validResponse[dev::ipc::FIELD_OK].get<bool>());
	ASSERT_TRUE(validResponse[dev::ipc::FIELD_DATA].contains("sp"));

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

static void test_get_mem_round_trip()
{
	ServerContext ctx;
	ASSERT_TRUE(ctx.Start());

	TestClient client;
	ASSERT_TRUE(client.Connect(ctx.port));
	ctx.WaitForClient();
	ctx.StartLoop();

	const uint32_t address = dev::Memory::MEMORY_GLOBAL_LEN - 3;
	const std::vector<uint8_t> expected = {0x12, 0x34, 0x56};
	for (size_t index = 0; index < expected.size(); ++index) {
		auto setResponse = client.SendRequest({
			{dev::ipc::FIELD_CMD, static_cast<int>(dev::Hardware::Req::SET_BYTE_GLOBAL)},
			{dev::ipc::FIELD_DATA, {{"addr", address + index}, {"data", expected[index]}}}
		});
		ASSERT_TRUE(setResponse[dev::ipc::FIELD_OK].get<bool>());
	}

	auto getResponse = client.SendRequest({
		{dev::ipc::FIELD_CMD, static_cast<int>(dev::Hardware::Req::GET_MEM)},
		{dev::ipc::FIELD_DATA, {{"addr", address}, {"len", expected.size()}}}
	});
	ASSERT_TRUE(getResponse[dev::ipc::FIELD_OK].get<bool>());
	ASSERT_EQ(getResponse[dev::ipc::FIELD_DATA]["addr"].get<uint32_t>(), address);
	ASSERT_TRUE(getResponse[dev::ipc::FIELD_DATA]["data"].get<std::vector<uint8_t>>() == expected);

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
static void test_hardware_command_ids()
{
	ASSERT_EQ(static_cast<int>(dev::Hardware::Req::SET_MEM), 42);
	ASSERT_EQ(static_cast<int>(dev::Hardware::Req::KEY_HANDLING), 47);
	ASSERT_EQ(static_cast<int>(dev::Hardware::Req::RUN_HEADLESS), 50);
	ASSERT_EQ(static_cast<int>(dev::Hardware::Req::DEBUG_BREAKPOINT_ADD), 60);
	ASSERT_EQ(static_cast<int>(dev::Hardware::Req::LOAD_ROM), 91);
	ASSERT_EQ(static_cast<int>(dev::Hardware::Req::MOUNT_FDD), 92);
	ASSERT_EQ(static_cast<int>(dev::Hardware::Req::GET_MEM), 93);
	ASSERT_EQ(static_cast<int>(dev::Hardware::Req::DEBUG_WATCHPOINT_EDIT), 94);
	ASSERT_EQ(static_cast<int>(dev::Hardware::Req::GET_STOP_RECORD), 95);
	ASSERT_EQ(static_cast<int>(dev::Hardware::Req::GET_HARDWARE_STATS), 96);
	ASSERT_EQ(static_cast<int>(dev::Hardware::Req::SET_IO_PALETTE_ENTRY), 97);
	ASSERT_EQ(static_cast<int>(dev::Hardware::Req::DISMOUNT_FDD), 98);

	auto hw = std::make_unique<dev::Hardware>("", "", true);
	ASSERT_TRUE(!hw->Request(static_cast<dev::Hardware::Req>(0)));
	ASSERT_TRUE(!hw->Request(static_cast<dev::Hardware::Req>(99)));
}

// ── Test: Protocol encode/decode round-trip ─────────────────────────
static void test_protocol_encode_decode()
{
	nlohmann::json original = {
		{"cmd", static_cast<int>(dev::Hardware::Req::SET_MEM)},
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
	ASSERT_EQ(decoded["cmd"].get<int>(), static_cast<int>(dev::Hardware::Req::SET_MEM));
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
	ASSERT_EQ(errResp[dev::ipc::FIELD_CODE].get<std::string>(), std::string("internal_error"));
	ASSERT_EQ(errResp[dev::ipc::FIELD_ERROR].get<std::string>(), std::string("something broke"));
}

static void test_request_validation()
{
	auto assertInvalid = [](const nlohmann::json& request) {
		auto result = dev::server::ValidateRequest(request);
		ASSERT_TRUE(std::holds_alternative<dev::server::RequestError>(result));
	};

	assertInvalid(nullptr);
	assertInvalid(nlohmann::json::object());
	assertInvalid({{dev::ipc::FIELD_CMD, "18"}, {dev::ipc::FIELD_DATA, {}}});
	assertInvalid({{dev::ipc::FIELD_CMD, 1000}, {dev::ipc::FIELD_DATA, {}}});
	assertInvalid({{dev::ipc::FIELD_CMD, dev::ipc::CMD_PONG}, {dev::ipc::FIELD_DATA, {}}});
	assertInvalid({{dev::ipc::FIELD_CMD, static_cast<int>(dev::Hardware::Req::GET_REGS)},
		{dev::ipc::FIELD_DATA, nullptr}});

	const auto stackCommand = static_cast<int>(dev::Hardware::Req::GET_STACK_SAMPLE);
	assertInvalid({{dev::ipc::FIELD_CMD, stackCommand}, {dev::ipc::FIELD_DATA, {}}});
	assertInvalid({{dev::ipc::FIELD_CMD, stackCommand}, {dev::ipc::FIELD_DATA, {{"addr", nullptr}}}});
	assertInvalid({{dev::ipc::FIELD_CMD, stackCommand}, {dev::ipc::FIELD_DATA, {{"addr", "65520"}}}});
	assertInvalid({{dev::ipc::FIELD_CMD, stackCommand}, {dev::ipc::FIELD_DATA, {{"addr", -1}}}});
	assertInvalid({{dev::ipc::FIELD_CMD, stackCommand}, {dev::ipc::FIELD_DATA, {{"addr", 1.5}}}});
	assertInvalid({{dev::ipc::FIELD_CMD, stackCommand}, {dev::ipc::FIELD_DATA, {{"addr", 65536}}}});

	const auto getMemCommand = static_cast<int>(dev::Hardware::Req::GET_MEM);
	const auto lastGlobalAddress = static_cast<uint32_t>(dev::Memory::MEMORY_GLOBAL_LEN - 1);
	assertInvalid({{dev::ipc::FIELD_CMD, getMemCommand}, {dev::ipc::FIELD_DATA, {}}});
	assertInvalid({{dev::ipc::FIELD_CMD, getMemCommand}, {dev::ipc::FIELD_DATA, {{"addr", -1}, {"len", 1}}}});
	assertInvalid({{dev::ipc::FIELD_CMD, getMemCommand}, {dev::ipc::FIELD_DATA, {{"addr", 0}, {"len", 0}}}});
	assertInvalid({{dev::ipc::FIELD_CMD, getMemCommand}, {dev::ipc::FIELD_DATA, {{"addr", lastGlobalAddress}, {"len", 2}}}});
	assertInvalid({{dev::ipc::FIELD_CMD, getMemCommand}, {dev::ipc::FIELD_DATA, {{"addr", dev::Memory::MEMORY_GLOBAL_LEN}, {"len", 1}}}});
	assertInvalid({{dev::ipc::FIELD_CMD, getMemCommand}, {dev::ipc::FIELD_DATA, {{"addr", 1.5}, {"len", 1}}}});
	assertInvalid({{dev::ipc::FIELD_CMD, getMemCommand}, {dev::ipc::FIELD_DATA, {{"addr", 0}, {"len", "1"}}}});

	for (const auto [address, length] : std::vector<std::pair<uint32_t, uint32_t>>{
		{0, 1}, {lastGlobalAddress, 1}}) {
		auto result = dev::server::ValidateRequest({
			{dev::ipc::FIELD_CMD, getMemCommand},
			{dev::ipc::FIELD_DATA, {{"addr", address}, {"len", length}}}
		});
		ASSERT_TRUE(std::holds_alternative<dev::server::IpcRequest>(result));
	}

	for (const int address : {0, 0xFFFF}) {
		auto result = dev::server::ValidateRequest({
			{dev::ipc::FIELD_CMD, stackCommand},
			{dev::ipc::FIELD_DATA, {{"addr", address}}}
		});
		ASSERT_TRUE(std::holds_alternative<dev::server::IpcRequest>(result));
	}

	auto infoResult = dev::server::ValidateRequest({
		{dev::ipc::FIELD_CMD, dev::ipc::CMD_GET_SERVER_INFO}
	});
	ASSERT_TRUE(std::holds_alternative<dev::server::IpcRequest>(infoResult));
	const auto stopRecordCommand = static_cast<int>(dev::Hardware::Req::GET_STOP_RECORD);
	ASSERT_TRUE(std::holds_alternative<dev::server::IpcRequest>(dev::server::ValidateRequest({
		{dev::ipc::FIELD_CMD, stopRecordCommand}, {dev::ipc::FIELD_DATA, nlohmann::json::object()}
	})));
	assertInvalid({{dev::ipc::FIELD_CMD, stopRecordCommand},
		{dev::ipc::FIELD_DATA, {{"consume", true}}}});

	const auto statsCommand = static_cast<int>(dev::Hardware::Req::GET_HARDWARE_STATS);
	ASSERT_TRUE(std::holds_alternative<dev::server::IpcRequest>(dev::server::ValidateRequest({
		{dev::ipc::FIELD_CMD, statsCommand}, {dev::ipc::FIELD_DATA, nlohmann::json::object()}
	})));
	assertInvalid({{dev::ipc::FIELD_CMD, statsCommand}, {dev::ipc::FIELD_DATA, {{"schema", 1}}}});

	const auto paletteCommand = static_cast<int>(dev::Hardware::Req::SET_IO_PALETTE_ENTRY);
	ASSERT_TRUE(std::holds_alternative<dev::server::IpcRequest>(dev::server::ValidateRequest({
		{dev::ipc::FIELD_CMD, paletteCommand}, {dev::ipc::FIELD_DATA, {{"index", 15}, {"hwColor", 255}}}
	})));
	assertInvalid({{dev::ipc::FIELD_CMD, paletteCommand}, {dev::ipc::FIELD_DATA, {{"index", 16}, {"hwColor", 0}}}});
	assertInvalid({{dev::ipc::FIELD_CMD, paletteCommand}, {dev::ipc::FIELD_DATA, {{"index", 0}, {"hwColor", 256}}}});
	assertInvalid({{dev::ipc::FIELD_CMD, paletteCommand}, {dev::ipc::FIELD_DATA, {{"index", 0}, {"hwColor", 1}, {"extra", true}}}});

	const auto dismountCommand = static_cast<int>(dev::Hardware::Req::DISMOUNT_FDD);
	ASSERT_TRUE(std::holds_alternative<dev::server::IpcRequest>(dev::server::ValidateRequest({
		{dev::ipc::FIELD_CMD, dismountCommand}, {dev::ipc::FIELD_DATA, {{"driveIdx", 3}}}
	})));
	assertInvalid({{dev::ipc::FIELD_CMD, dismountCommand}, {dev::ipc::FIELD_DATA, {{"driveIdx", 4}}}});
	assertInvalid({{dev::ipc::FIELD_CMD, dismountCommand}, {dev::ipc::FIELD_DATA, {{"driveIdx", 0}, {"save", false}}}});

	const auto watchpointAdd = static_cast<int>(dev::Hardware::Req::DEBUG_WATCHPOINT_ADD);
	const nlohmann::json watchpoint = {
		{"globalAddr", 65536}, {"len", 4}, {"value", 32},
		{"access", "RW"}, {"condition", "EQU"}, {"type", "LEN"},
		{"active", true}, {"comment", "screen buffer"}
	};
	auto watchpointResult = dev::server::ValidateRequest({
		{dev::ipc::FIELD_CMD, watchpointAdd}, {dev::ipc::FIELD_DATA, watchpoint}
	});
	ASSERT_TRUE(std::holds_alternative<dev::server::IpcRequest>(watchpointResult));
	const auto watchpointEdit = static_cast<int>(dev::Hardware::Req::DEBUG_WATCHPOINT_EDIT);
	auto editedWatchpoint = watchpoint;
	editedWatchpoint["id"] = 7;
	auto watchpointEditResult = dev::server::ValidateRequest({
		{dev::ipc::FIELD_CMD, watchpointEdit}, {dev::ipc::FIELD_DATA, editedWatchpoint}
	});
	ASSERT_TRUE(std::holds_alternative<dev::server::IpcRequest>(watchpointEditResult));
	auto missingEditId = watchpoint;
	assertInvalid({{dev::ipc::FIELD_CMD, watchpointEdit}, {dev::ipc::FIELD_DATA, missingEditId}});
	editedWatchpoint["id"] = -1;
	assertInvalid({{dev::ipc::FIELD_CMD, watchpointEdit}, {dev::ipc::FIELD_DATA, editedWatchpoint}});

	auto assertInvalidWatchpoint = [&watchpoint, &assertInvalid, watchpointAdd](const nlohmann::json& patch) {
		auto invalidData = watchpoint;
		invalidData.update(patch);
		assertInvalid({{dev::ipc::FIELD_CMD, watchpointAdd}, {dev::ipc::FIELD_DATA, invalidData}});
	};
	assertInvalidWatchpoint({{"id", 7}});
	assertInvalidWatchpoint({{"schemaVersion", 1}});
	assertInvalidWatchpoint({{"globalAddr", dev::Memory::MEMORY_GLOBAL_LEN}});
	assertInvalidWatchpoint({{"len", 0}});
	assertInvalidWatchpoint({{"value", 256}});
	assertInvalidWatchpoint({{"access", "EXEC"}});
	assertInvalidWatchpoint({{"condition", "="}});
	assertInvalidWatchpoint({{"type", "WORD"}});
	assertInvalidWatchpoint({{"active", 1}});
	assertInvalidWatchpoint({{"comment", std::string(1025, 'x')}});
	assertInvalidWatchpoint({{"comment", std::string("\xC3\x28", 2)}});
	assertInvalid({{dev::ipc::FIELD_CMD, watchpointAdd},
		{dev::ipc::FIELD_DATA, {{"data0", 0u}, {"data1", 0u}, {"comment", "legacy"}}}});

	auto invalidLength = watchpoint;
	invalidLength["len"] = 0;
	auto fieldErrorResult = dev::server::ValidateRequest({
		{dev::ipc::FIELD_CMD, watchpointAdd},
		{dev::ipc::FIELD_DATA, invalidLength}
	});
	ASSERT_TRUE(std::holds_alternative<dev::server::RequestError>(fieldErrorResult));
	const auto& fieldError = std::get<dev::server::RequestError>(fieldErrorResult);
	ASSERT_EQ(fieldError.details["command"].get<int>(), watchpointAdd);
	ASSERT_EQ(fieldError.details["field"].get<std::string>(), std::string("len"));

	const auto watchpointGetAll = static_cast<int>(dev::Hardware::Req::DEBUG_WATCHPOINT_GET_ALL);
	assertInvalid({{dev::ipc::FIELD_CMD, watchpointGetAll},
		{dev::ipc::FIELD_DATA, {{"schemaVersion", 1}}}});

	const auto breakpointAdd = static_cast<int>(dev::Hardware::Req::DEBUG_BREAKPOINT_ADD);
	const nlohmann::json breakpoint = {
		{"addr", 0x1234}, {"memPages", dev::Breakpoint::MAPPING_PAGES_ALL},
		{"status", "ACTIVE"}, {"autoDelete", false}, {"operand", "A"},
		{"condition", "ANY"}, {"value", 0}, {"comment", "entry"}
	};
	auto breakpointResult = dev::server::ValidateRequest({
		{dev::ipc::FIELD_CMD, breakpointAdd}, {dev::ipc::FIELD_DATA, breakpoint}
	});
	ASSERT_TRUE(std::holds_alternative<dev::server::IpcRequest>(breakpointResult));
	auto assertInvalidBreakpoint = [&breakpoint, &assertInvalid, breakpointAdd](const nlohmann::json& patch) {
		auto invalidData = breakpoint;
		invalidData.update(patch);
		assertInvalid({{dev::ipc::FIELD_CMD, breakpointAdd}, {dev::ipc::FIELD_DATA, invalidData}});
	};
	assertInvalidBreakpoint({{"data0", 0u}});
	assertInvalidBreakpoint({{"addr", 65536}});
	assertInvalidBreakpoint({{"memPages", 0}});
	assertInvalidBreakpoint({{"memPages", dev::Breakpoint::MAPPING_PAGES_ALL + 1}});
	assertInvalidBreakpoint({{"status", "DELETED"}});
	assertInvalidBreakpoint({{"autoDelete", 0}});
	assertInvalidBreakpoint({{"operand", "Flags"}});
	assertInvalidBreakpoint({{"condition", "="}});
	assertInvalidBreakpoint({{"value", 256}});
	assertInvalidBreakpoint({{"comment", std::string(1025, 'x')}});

	const auto breakpointSetStatus = static_cast<int>(dev::Hardware::Req::DEBUG_BREAKPOINT_SET_STATUS);
	auto statusResult = dev::server::ValidateRequest({
		{dev::ipc::FIELD_CMD, breakpointSetStatus},
		{dev::ipc::FIELD_DATA, {{"addr", 0x1234}, {"status", "DISABLED"}}}
	});
	ASSERT_TRUE(std::holds_alternative<dev::server::IpcRequest>(statusResult));
	assertInvalid({{dev::ipc::FIELD_CMD, breakpointSetStatus},
		{dev::ipc::FIELD_DATA, {{"addr", 0x1234}, {"status", 0}}}});
	assertInvalid({{dev::ipc::FIELD_CMD, static_cast<int>(dev::Hardware::Req::DEBUG_BREAKPOINT_GET_ALL)},
		{dev::ipc::FIELD_DATA, {{"schemaVersion", 1}}}});
}

static void test_server_info()
{
	auto info = dev::server::MakeServerInfo("test-build");
	auto hasCommand = [&info](const int command) {
		return std::find(info["commands"].begin(), info["commands"].end(), command) !=
			info["commands"].end();
	};
	ASSERT_EQ(info["protocolVersion"].get<int>(), dev::ipc::PROTOCOL_VERSION);
	ASSERT_EQ(info["emulatorVersion"].get<std::string>(), std::string("test-build"));
	ASSERT_TRUE(hasCommand(dev::ipc::CMD_GET_SERVER_INFO));
	ASSERT_TRUE(hasCommand(static_cast<int>(dev::Hardware::Req::GET_STACK_SAMPLE)));
	ASSERT_TRUE(hasCommand(static_cast<int>(dev::Hardware::Req::GET_MEM)));
	ASSERT_TRUE(hasCommand(static_cast<int>(dev::Hardware::Req::GET_STOP_RECORD)));
	ASSERT_TRUE(hasCommand(static_cast<int>(dev::Hardware::Req::GET_HARDWARE_STATS)));
	ASSERT_TRUE(hasCommand(static_cast<int>(dev::Hardware::Req::SET_IO_PALETTE_ENTRY)));
	ASSERT_TRUE(hasCommand(static_cast<int>(dev::Hardware::Req::DISMOUNT_FDD)));
	ASSERT_TRUE(info["capabilities"]["debugger"].get<bool>());
	ASSERT_EQ(info["capabilities"]["rawFrameSchema"].get<int>(), 1);
	ASSERT_EQ(info["capabilities"]["stackSampleSchema"].get<int>(), 1);
	ASSERT_EQ(info["capabilities"]["breakpointSchema"].get<int>(), 1);
	ASSERT_EQ(info["capabilities"]["breakpointLimits"]["mappingPageBits"].get<int>(), 33);
	ASSERT_EQ(info["capabilities"]["watchpointSchema"].get<int>(), 1);
	ASSERT_EQ(info["capabilities"]["stopRecordSchema"].get<int>(), 1);
	ASSERT_EQ(info["capabilities"]["hardwareStatsSchema"].get<int>(), 1);
	ASSERT_TRUE(info["capabilities"]["hardwareStatsWhileRunning"].get<bool>());
	ASSERT_TRUE(!info["capabilities"]["runningHardwareMutations"].get<bool>());
	ASSERT_TRUE(!info["capabilities"].contains("legacyPackedWatchpoints"));
	ASSERT_TRUE(info["capabilities"]["watchpointServerAllocatedIds"].get<bool>());
	ASSERT_TRUE(info["capabilities"]["watchpointEdit"].get<bool>());
	ASSERT_EQ(info["capabilities"]["watchpointLimits"]["maxCommentBytes"].get<int>(), 1024);

	auto response = dev::ipc::MakeResponse(info);
	ASSERT_TRUE(dev::ipc::IsRawFrameServerCompatible(response));
	response[dev::ipc::FIELD_DATA]["protocolVersion"] = dev::ipc::PROTOCOL_VERSION - 1;
	ASSERT_TRUE(!dev::ipc::IsRawFrameServerCompatible(response));
	response[dev::ipc::FIELD_DATA]["protocolVersion"] = dev::ipc::PROTOCOL_VERSION;
	response[dev::ipc::FIELD_DATA]["capabilities"]["rawFrameSchema"] = 0;
	ASSERT_TRUE(!dev::ipc::IsRawFrameServerCompatible(response));
	response[dev::ipc::FIELD_DATA]["capabilities"]["rawFrameSchema"] =
		dev::ipc::RAW_FRAME_SCHEMA_VERSION;
	response[dev::ipc::FIELD_DATA]["commands"] = nlohmann::json::array();
	ASSERT_TRUE(!dev::ipc::IsRawFrameServerCompatible(response));
	response[dev::ipc::FIELD_DATA]["protocolVersion"] = "2";
	ASSERT_TRUE(!dev::ipc::IsRawFrameServerCompatible(response));
	ASSERT_TRUE(!dev::ipc::IsRawFrameServerCompatible(nullptr));
}

static void test_stop_record_lifecycle()
{
	auto hw = std::make_unique<dev::Hardware>("", "", true);
	auto readRecord = [&hw]() {
		return *hw->Request(dev::Hardware::Req::GET_STOP_RECORD);
	};

	const auto initial = readRecord();
	ASSERT_EQ(initial["sequence"].get<uint64_t>(), uint64_t(0));
	ASSERT_EQ(initial["reason"].get<std::string>(), std::string("unknown"));
	ASSERT_EQ(readRecord(), initial);

	ASSERT_TRUE(hw->Request(dev::Hardware::Req::STOP));
	const auto paused = readRecord();
	ASSERT_EQ(paused["sequence"].get<uint64_t>(), uint64_t(1));
	ASSERT_EQ(paused["reason"].get<std::string>(), std::string("pause"));
	ASSERT_EQ(readRecord(), paused);

	ASSERT_TRUE(hw->Request(dev::Hardware::Req::RESET));
	ASSERT_EQ(readRecord(), paused);

	ASSERT_TRUE(hw->Request(dev::Hardware::Req::SET_MEM,
		{{"addr", 0}, {"data", std::vector<uint8_t>{0x00, 0x76}}}));
	ASSERT_TRUE(hw->Request(dev::Hardware::Req::RESTART));
	ASSERT_EQ(readRecord(), paused);
	ASSERT_TRUE(hw->Request(dev::Hardware::Req::EXECUTE_INSTR));
	const auto stepped = readRecord();
	ASSERT_EQ(stepped["reason"].get<std::string>(), std::string("step"));
	ASSERT_EQ(stepped["sequence"].get<uint64_t>(), uint64_t(2));
	ASSERT_TRUE(hw->Request(dev::Hardware::Req::EXECUTE_INSTR));
	const auto haltStep = readRecord();
	ASSERT_EQ(haltStep["reason"].get<std::string>(), std::string("step"));
	ASSERT_EQ(haltStep["sequence"].get<uint64_t>(), uint64_t(3));

	ASSERT_TRUE(hw->Request(dev::Hardware::Req::RUN));
	const auto running = *hw->Request(dev::Hardware::Req::IS_RUNNING);
	ASSERT_TRUE(running["isRunning"].get<bool>());
	ASSERT_EQ(readRecord(), haltStep);
	ASSERT_TRUE(hw->Request(dev::Hardware::Req::STOP));
}

static void test_structured_breakpoints()
{
	dev::Breakpoint lastRamDiskPage{
		dev::Breakpoint::Data{
			0x1234, dev::Breakpoint::MemPages{uint64_t{1} << 32},
			dev::Breakpoint::Status::ACTIVE, false, dev::Breakpoint::Operand::A,
			dev::Condition::ANY, 0
		}
	};
	dev::CpuI8080::State cpuState{};
	auto ram = std::make_unique<dev::Memory::Ram>();
	dev::Memory::State memoryState{dev::Memory::GetGlobalAddrFunc{}, ram.get()};
	memoryState.update.mapping.modeRamA = true;
	memoryState.update.mapping.pageRam = 3;
	memoryState.update.ramdiskIdx = 7;
	ASSERT_TRUE(lastRamDiskPage.CheckStatus(cpuState, memoryState));
	memoryState.update.mapping.data = 0;
	ASSERT_TRUE(!lastRamDiskPage.CheckStatus(cpuState, memoryState));

	auto hw = std::make_unique<dev::Hardware>("", "", true);
	auto debugger = std::make_unique<dev::Debugger>(*hw, 1);
	auto getAll = [&hw]() {
		return hw->Request(dev::Hardware::Req::DEBUG_BREAKPOINT_GET_ALL);
	};
	auto updates = [&hw]() {
		return (*hw->Request(dev::Hardware::Req::DEBUG_BREAKPOINT_GET_UPDATES))["updates"].get<uint32_t>();
	};
	auto add = [&hw](const uint16_t address, const std::string& operand) {
		return hw->Request(dev::Hardware::Req::DEBUG_BREAKPOINT_ADD, {
			{"addr", address}, {"memPages", dev::Breakpoint::MAPPING_PAGES_ALL},
			{"status", "ACTIVE"}, {"autoDelete", false}, {"operand", operand},
			{"condition", "ANY"}, {"value", 0}, {"comment", "test"}
		});
	};

	auto empty = getAll();
	ASSERT_TRUE(empty.HasValue());
	ASSERT_TRUE(empty->is_array());
	ASSERT_TRUE(empty->empty());
	const auto initialUpdates = updates();
	ASSERT_TRUE(hw->Request(dev::Hardware::Req::DEBUG_BREAKPOINT_DEL_ALL));
	ASSERT_EQ(updates(), initialUpdates);

	ASSERT_TRUE(add(0x2000, "A"));
	ASSERT_TRUE(add(0x1000, "F"));
	auto all = getAll();
	ASSERT_TRUE(all.HasValue());
	ASSERT_EQ(all->size(), static_cast<size_t>(2));
	ASSERT_EQ((*all)[0]["addr"].get<uint16_t>(), static_cast<uint16_t>(0x1000));
	ASSERT_EQ((*all)[1]["addr"].get<uint16_t>(), static_cast<uint16_t>(0x2000));
	ASSERT_EQ((*all)[0]["operand"].get<std::string>(), std::string("F"));
	ASSERT_EQ((*all)[0]["status"].get<std::string>(), std::string("ACTIVE"));
	ASSERT_EQ((*all)[0]["memPages"].get<uint64_t>(), dev::Breakpoint::MAPPING_PAGES_ALL);
	ASSERT_TRUE(!(*all)[0].contains("data0"));

	ASSERT_TRUE(hw->Request(dev::Hardware::Req::DEBUG_BREAKPOINT_SET_STATUS,
		{{"addr", 0x1000}, {"status", "DISABLED"}}));
	auto status = hw->Request(dev::Hardware::Req::DEBUG_BREAKPOINT_GET_STATUS, {{"addr", 0x1000}});
	ASSERT_EQ((*status)["status"].get<std::string>(), std::string("DISABLED"));
	const auto beforeNoOp = updates();
	ASSERT_TRUE(hw->Request(dev::Hardware::Req::DEBUG_BREAKPOINT_SET_STATUS,
		{{"addr", 0x1000}, {"status", "DISABLED"}}));
	ASSERT_TRUE(hw->Request(dev::Hardware::Req::DEBUG_BREAKPOINT_DEL, {{"addr", 0x9999}}));
	ASSERT_EQ(updates(), beforeNoOp);
}

static void test_structured_watchpoints()
{
	auto hw = std::make_unique<dev::Hardware>("", "", true);
	auto debugger = std::make_unique<dev::Debugger>(*hw, 1);
	auto getAll = [&hw]() {
		return hw->Request(dev::Hardware::Req::DEBUG_WATCHPOINT_GET_ALL);
	};
	auto updates = [&hw]() {
		return (*hw->Request(dev::Hardware::Req::DEBUG_WATCHPOINT_GET_UPDATES))["updates"].get<uint32_t>();
	};

	auto empty = getAll();
	ASSERT_TRUE(empty.HasValue());
	ASSERT_TRUE(empty->is_array());
	ASSERT_TRUE(empty->empty());
	const auto initialUpdates = updates();
	ASSERT_TRUE(hw->Request(dev::Hardware::Req::DEBUG_WATCHPOINT_DEL_ALL));
	ASSERT_EQ(updates(), initialUpdates);

	auto add = [&hw](const uint32_t address, const std::string& comment) {
		return hw->Request(dev::Hardware::Req::DEBUG_WATCHPOINT_ADD, {
			{"globalAddr", address}, {"len", 1}, {"value", 0x20},
			{"access", "RW"}, {"condition", "EQU"}, {"type", "LEN"},
			{"active", true}, {"comment", comment}
		});
	};
	ASSERT_TRUE(add(0x20000, "second"));
	ASSERT_TRUE(add(0x10000, "first"));
	ASSERT_EQ(updates(), initialUpdates + 2);

	auto all = getAll();
	ASSERT_TRUE(all.HasValue());
	ASSERT_EQ(all->size(), static_cast<size_t>(2));
	ASSERT_EQ((*all)[0]["id"].get<int>(), 0);
	ASSERT_EQ((*all)[1]["id"].get<int>(), 1);
	ASSERT_EQ((*all)[0]["globalAddr"].get<uint32_t>(), static_cast<uint32_t>(0x20000));
	ASSERT_EQ((*all)[0]["condition"].get<std::string>(), std::string("EQU"));
	ASSERT_TRUE(!(*all)[0].contains("breakL"));
	ASSERT_TRUE(!(*all)[0].contains("breakH"));

	const auto beforeReset = updates();
	ASSERT_TRUE(hw->Request(dev::Hardware::Req::RESET));
	all = getAll();
	ASSERT_EQ(all->size(), static_cast<size_t>(2));
	ASSERT_EQ((*all)[0]["id"].get<int>(), 0);
	ASSERT_EQ((*all)[1]["id"].get<int>(), 1);
	ASSERT_EQ(updates(), beforeReset);

	ASSERT_TRUE(hw->Request(dev::Hardware::Req::RESTART));
	all = getAll();
	ASSERT_EQ(all->size(), static_cast<size_t>(2));
	ASSERT_EQ((*all)[0]["id"].get<int>(), 0);
	ASSERT_EQ((*all)[1]["id"].get<int>(), 1);
	ASSERT_EQ(updates(), beforeReset);

	const auto beforeEdit = updates();
	ASSERT_TRUE(hw->Request(dev::Hardware::Req::DEBUG_WATCHPOINT_EDIT, {
		{"id", 0}, {"globalAddr", 0x30000}, {"len", 2}, {"value", 0x1234},
		{"access", "W"}, {"condition", "NOT_EQU"}, {"type", "WORD"},
		{"active", false}, {"comment", "edited"}
	}));
	ASSERT_EQ(updates(), beforeEdit + 1);
	all = getAll();
	ASSERT_EQ((*all)[0]["id"].get<int>(), 0);
	ASSERT_EQ((*all)[0]["globalAddr"].get<uint32_t>(), static_cast<uint32_t>(0x30000));
	ASSERT_EQ((*all)[0]["comment"].get<std::string>(), std::string("edited"));

	const auto beforeInvalidEdit = updates();
	bool notFound = false;
	try {
		hw->Request(dev::Hardware::Req::DEBUG_WATCHPOINT_EDIT, {
			{"id", 999}, {"globalAddr", 0x30000}, {"len", 1}, {"value", 0x12},
			{"access", "R"}, {"condition", "EQU"}, {"type", "LEN"},
			{"active", true}, {"comment", "missing"}
		});
	} catch (const dev::WatchpointNotFound& error) {
		notFound = error.GetId() == 999;
	}
	ASSERT_TRUE(notFound);
	ASSERT_EQ(updates(), beforeInvalidEdit);

	const auto beforeMissingDelete = updates();
	ASSERT_TRUE(hw->Request(dev::Hardware::Req::DEBUG_WATCHPOINT_DEL, {{"id", 999}}));
	ASSERT_EQ(updates(), beforeMissingDelete);

	ASSERT_TRUE(hw->Request(dev::Hardware::Req::DEBUG_WATCHPOINT_DEL, {{"id", 1}}));
	ASSERT_EQ(updates(), beforeMissingDelete + 1);
	all = getAll();
	ASSERT_EQ(all->size(), static_cast<size_t>(1));
	ASSERT_EQ((*all)[0]["id"].get<int>(), 0);

	ASSERT_TRUE(add(0x40000, "third"));
	all = getAll();
	ASSERT_EQ(all->size(), static_cast<size_t>(2));
	ASSERT_EQ((*all)[0]["id"].get<int>(), 0);
	ASSERT_EQ((*all)[1]["id"].get<int>(), 2);

	const auto beforeClear = updates();
	ASSERT_TRUE(hw->Request(dev::Hardware::Req::DEBUG_WATCHPOINT_DEL_ALL));
	ASSERT_EQ(updates(), beforeClear + 1);
	all = getAll();
	ASSERT_TRUE(all->empty());
	ASSERT_TRUE(hw->Request(dev::Hardware::Req::DEBUG_WATCHPOINT_DEL_ALL));
	ASSERT_EQ(updates(), beforeClear + 1);
}

static void test_watchpoint_matching()
{
	dev::Watchpoints watchpoints;
	const auto readId = watchpoints.AddNew(dev::Watchpoint{
		dev::Watchpoint::Data{-1, dev::Watchpoint::Access::R, 0x1000,
			dev::Condition::EQU, 0x42, dev::Watchpoint::Type::LEN, 2, true},
		"read range"
	});
	const auto wordId = watchpoints.AddNew(dev::Watchpoint{
		dev::Watchpoint::Data{-1, dev::Watchpoint::Access::W, 0x2000,
			dev::Condition::EQU, 0x1234, dev::Watchpoint::Type::WORD, 2, true},
		"word write"
	});
	const auto disabledId = watchpoints.AddNew(dev::Watchpoint{
		dev::Watchpoint::Data{-1, dev::Watchpoint::Access::RW, 0x3000,
			dev::Condition::ANY, 0, dev::Watchpoint::Type::LEN, 1, false},
		"disabled"
	});
	ASSERT_EQ(readId, 0);
	ASSERT_EQ(wordId, 1);
	ASSERT_EQ(disabledId, 2);
	ASSERT_EQ(watchpoints.GetAll().size(), static_cast<size_t>(3));

	watchpoints.Check(dev::Watchpoint::Access::W, 0x1000, 0x42);
	ASSERT_TRUE(!watchpoints.CheckBreak());
	watchpoints.Check(dev::Watchpoint::Access::R, 0x1001, 0x41);
	ASSERT_TRUE(!watchpoints.CheckBreak());
	watchpoints.Check(dev::Watchpoint::Access::R, 0x1001, 0x42);
	ASSERT_TRUE(watchpoints.CheckBreak());
	ASSERT_TRUE(!watchpoints.CheckBreak());

	watchpoints.Check(dev::Watchpoint::Access::W, 0x2000, 0x34);
	ASSERT_TRUE(!watchpoints.CheckBreak());
	watchpoints.Check(dev::Watchpoint::Access::W, 0x2001, 0x12);
	ASSERT_TRUE(watchpoints.CheckBreak());
	watchpoints.Check(dev::Watchpoint::Access::R, 0x2000, 0x34);
	watchpoints.Check(dev::Watchpoint::Access::R, 0x2001, 0x12);
	ASSERT_TRUE(!watchpoints.CheckBreak());

	watchpoints.Check(dev::Watchpoint::Access::R, 0x3000, 0xFF);
	ASSERT_TRUE(!watchpoints.CheckBreak());

	dev::Watchpoints overlapping;
	overlapping.AddNew(dev::Watchpoint{
		dev::Watchpoint::Data{-1, dev::Watchpoint::Access::R, 0x4000,
			dev::Condition::ANY, 0, dev::Watchpoint::Type::LEN, 1, true},
		"first"
	});
	const auto overlappingWordId = overlapping.AddNew(dev::Watchpoint{
		dev::Watchpoint::Data{-1, dev::Watchpoint::Access::R, 0x4000,
			dev::Condition::EQU, 0x1234, dev::Watchpoint::Type::WORD, 2, true},
		"second"
	});
	overlapping.Check(dev::Watchpoint::Access::R, 0x4000, 0x34);
	ASSERT_TRUE(overlapping.GetAll().at(overlappingWordId).data.breakL);
}

static void test_watchpoints_break_execution()
{
	struct Scenario {
		const char* name;
		std::vector<uint8_t> program;
		std::vector<uint8_t> targetData;
		uint32_t globalAddr;
		uint8_t len;
		uint16_t value;
		const char* access;
		const char* condition;
		const char* type;
		bool active;
		bool shouldBreak;
		uint16_t breakPc;
	};

	const std::vector<uint8_t> writeByte = {
		0x3E, 0x42,             // MVI A, 0x42
		0x32, 0x00, 0x10,       // STA 0x1000
		0xC3, 0x00, 0x00        // JMP 0
	};
	const std::vector<uint8_t> readByte = {
		0x3A, 0x00, 0x10,       // LDA 0x1000
		0xC3, 0x00, 0x00        // JMP 0
	};
	const std::vector<uint8_t> writeWord = {
		0x21, 0x34, 0x12,       // LXI H, 0x1234
		0x22, 0x00, 0x10,       // SHLD 0x1000
		0xC3, 0x00, 0x00        // JMP 0
	};
	const std::vector<uint8_t> readWord = {
		0x2A, 0x00, 0x10,       // LHLD 0x1000
		0xC3, 0x00, 0x00        // JMP 0
	};

	const std::vector<Scenario> scenarios = {
		{"write ANY", writeByte, {0}, 0x1000, 1, 0, "W", "ANY", "LEN", true, true, 5},
		{"write EQU", writeByte, {0}, 0x1000, 1, 0x42, "W", "EQU", "LEN", true, true, 5},
		{"write LESS", writeByte, {0}, 0x1000, 1, 0x43, "W", "LESS", "LEN", true, true, 5},
		{"write GREATER", writeByte, {0}, 0x1000, 1, 0x41, "W", "GREATER", "LEN", true, true, 5},
		{"write LESS_EQU", writeByte, {0}, 0x1000, 1, 0x42, "W", "LESS_EQU", "LEN", true, true, 5},
		{"write GREATER_EQU", writeByte, {0}, 0x1000, 1, 0x42, "W", "GREATER_EQU", "LEN", true, true, 5},
		{"write NOT_EQU", writeByte, {0}, 0x1000, 1, 0x43, "W", "NOT_EQU", "LEN", true, true, 5},
		{"read access", readByte, {0x42}, 0x1000, 1, 0x42, "R", "EQU", "LEN", true, true, 3},
		{"read-write access", writeByte, {0}, 0x1000, 1, 0x42, "RW", "EQU", "LEN", true, true, 5},
		{"range", writeByte, {0}, 0x0FFF, 2, 0x42, "W", "EQU", "LEN", true, true, 5},
		{"word write", writeWord, {0, 0}, 0x1000, 2, 0x1234, "W", "EQU", "WORD", true, true, 6},
		{"word read", readWord, {0x34, 0x12}, 0x1000, 2, 0x1234, "R", "EQU", "WORD", true, true, 3},
		{"condition mismatch", writeByte, {0}, 0x1000, 1, 0x43, "W", "EQU", "LEN", true, false, 0},
		{"access mismatch", writeByte, {0}, 0x1000, 1, 0x42, "R", "EQU", "LEN", true, false, 0},
		{"inactive", writeByte, {0}, 0x1000, 1, 0x42, "W", "EQU", "LEN", false, false, 0},
		{"word mismatch", writeWord, {0, 0}, 0x1000, 2, 0x1334, "W", "EQU", "WORD", true, false, 0},
	};

	for (const auto& scenario : scenarios) {
		auto hw = std::make_unique<dev::Hardware>("", "", true);
		auto debugger = std::make_unique<dev::Debugger>(*hw, 1);
		ASSERT_TRUE(hw->Request(dev::Hardware::Req::DEBUG_ATTACH, {{"data", true}}));
		ASSERT_TRUE(hw->Request(dev::Hardware::Req::SET_CPU_SPEED,
			{{"speed", static_cast<int>(dev::Hardware::ExecSpeed::MAX)}}));
		ASSERT_TRUE(hw->Request(dev::Hardware::Req::SET_MEM,
			{{"addr", 0}, {"data", scenario.program}}));
		ASSERT_TRUE(hw->Request(dev::Hardware::Req::SET_MEM,
			{{"addr", 0x1000}, {"data", scenario.targetData}}));
		ASSERT_TRUE(hw->Request(dev::Hardware::Req::RESTART));
		ASSERT_TRUE(hw->Request(dev::Hardware::Req::DEBUG_WATCHPOINT_ADD, {
			{"globalAddr", scenario.globalAddr}, {"len", scenario.len},
			{"value", scenario.value}, {"access", scenario.access},
			{"condition", scenario.condition}, {"type", scenario.type},
			{"active", scenario.active}, {"comment", scenario.name}
		}));
		ASSERT_TRUE(hw->Request(dev::Hardware::Req::RUN));

		bool running = true;
		for (int poll = 0; poll < 32 && running; ++poll) {
			auto status = hw->Request(dev::Hardware::Req::IS_RUNNING);
			ASSERT_TRUE(status);
			running = (*status)["isRunning"].get<bool>();
		}
		ASSERT_EQ(running, !scenario.shouldBreak);
		if (scenario.shouldBreak) {
			auto pc = hw->Request(dev::Hardware::Req::GET_REG_PC);
			ASSERT_TRUE(pc);
			ASSERT_EQ((*pc)["pc"].get<uint16_t>(), scenario.breakPc);
			auto record = hw->Request(dev::Hardware::Req::GET_STOP_RECORD);
			ASSERT_TRUE(record);
			ASSERT_EQ((*record)["reason"].get<std::string>(), std::string("watchpoint"));
			ASSERT_EQ((*record)["watchpointIds"][0].get<int>(), 0);
			ASSERT_EQ((*record)["accessedGlobalAddress"].get<uint32_t>(), scenario.globalAddr + scenario.len - 1);
			ASSERT_EQ((*record)["access"].get<std::string>(),
				std::string(scenario.breakPc == 3 ? "read" : "write"));
			ASSERT_TRUE(record->contains(scenario.breakPc == 3 ? "observedValue" : "newValue"));
		} else {
			ASSERT_TRUE(hw->Request(dev::Hardware::Req::STOP));
		}
	}
}

static void test_breakpoint_stop_record()
{
	auto hw = std::make_unique<dev::Hardware>("", "", true);
	auto debugger = std::make_unique<dev::Debugger>(*hw, 1);
	ASSERT_TRUE(hw->Request(dev::Hardware::Req::DEBUG_ATTACH, {{"data", true}}));
	ASSERT_TRUE(hw->Request(dev::Hardware::Req::SET_CPU_SPEED,
		{{"speed", static_cast<int>(dev::Hardware::ExecSpeed::MAX)}}));
	ASSERT_TRUE(hw->Request(dev::Hardware::Req::SET_MEM,
		{{"addr", 0}, {"data", std::vector<uint8_t>{0x00, 0xC3, 0x00, 0x00}}}));
	ASSERT_TRUE(hw->Request(dev::Hardware::Req::RESTART));
	ASSERT_TRUE(hw->Request(dev::Hardware::Req::DEBUG_BREAKPOINT_ADD, {
		{"addr", 1}, {"memPages", dev::Breakpoint::MAPPING_PAGES_ALL},
		{"status", "ACTIVE"}, {"autoDelete", false}, {"operand", "A"},
		{"condition", "ANY"}, {"value", 0}, {"comment", "after NOP"}
	}));
	ASSERT_TRUE(hw->Request(dev::Hardware::Req::RUN));

	bool running = true;
	for (int poll = 0; poll < 32 && running; ++poll) {
		running = (*hw->Request(dev::Hardware::Req::IS_RUNNING))["isRunning"].get<bool>();
	}
	ASSERT_TRUE(!running);
	const auto record = *hw->Request(dev::Hardware::Req::GET_STOP_RECORD);
	ASSERT_EQ(record["reason"].get<std::string>(), std::string("breakpoint"));
	ASSERT_EQ(record["breakpointAddress"].get<uint16_t>(), uint16_t(1));
	ASSERT_EQ(record["breakpointIds"][0].get<uint16_t>(), uint16_t(1));
	ASSERT_EQ(record["pc"].get<uint16_t>(), uint16_t(1));
}

static void test_raw_frame_codec()
{
	const std::vector<uint8_t> pixels = {1, 2, 3, 4, 5, 6, 7, 8};
	std::vector<uint8_t> frame;
	ASSERT_TRUE(dev::ipc::EncodeRawFrame(frame, 2, 1, pixels.data(), pixels.size()));
	ASSERT_EQ(frame.size(), static_cast<size_t>(4 + dev::ipc::RAW_FRAME_HEADER_SIZE + pixels.size()));
	auto frameHeader = dev::ipc::DecodeRawFrameHeader(
		std::span<const uint8_t>(frame.data() + 4, dev::ipc::RAW_FRAME_HEADER_SIZE));
	ASSERT_TRUE(frameHeader.has_value());
	ASSERT_TRUE(frameHeader->kind == dev::ipc::RawFrameKind::FRAME);
	ASSERT_EQ(frameHeader->value0, static_cast<uint32_t>(2));
	ASSERT_EQ(frameHeader->value1, static_cast<uint32_t>(1));
	ASSERT_TRUE(std::equal(pixels.begin(), pixels.end(),
		frame.begin() + 4 + dev::ipc::RAW_FRAME_HEADER_SIZE));
	ASSERT_EQ(frame[0], static_cast<uint8_t>(dev::ipc::RAW_FRAME_HEADER_SIZE + pixels.size()));
	ASSERT_EQ(frame[1], static_cast<uint8_t>(0));

	const std::string message = "no frame available";
	std::vector<uint8_t> error;
	ASSERT_TRUE(dev::ipc::EncodeRawFrameError(error,
		dev::ipc::RawFrameErrorCode::FRAME_UNAVAILABLE, message));
	auto errorHeader = dev::ipc::DecodeRawFrameHeader(
		std::span<const uint8_t>(error.data() + 4, dev::ipc::RAW_FRAME_HEADER_SIZE));
	ASSERT_TRUE(errorHeader.has_value());
	ASSERT_TRUE(errorHeader->kind == dev::ipc::RawFrameKind::ERROR_RESPONSE);
	ASSERT_EQ(errorHeader->value0,
		static_cast<uint32_t>(dev::ipc::RawFrameErrorCode::FRAME_UNAVAILABLE));
	ASSERT_EQ(errorHeader->value1, static_cast<uint32_t>(message.size()));
	ASSERT_EQ(std::string(error.begin() + 4 + dev::ipc::RAW_FRAME_HEADER_SIZE, error.end()), message);

	auto invalidHeader = std::vector<uint8_t>(dev::ipc::RAW_FRAME_HEADER_SIZE, 0);
	ASSERT_TRUE(!dev::ipc::DecodeRawFrameHeader(invalidHeader));

	auto malformedHeader = std::vector<uint8_t>(
		frame.begin() + 4, frame.begin() + 4 + dev::ipc::RAW_FRAME_HEADER_SIZE);
	malformedHeader[4] = 2;
	ASSERT_TRUE(!dev::ipc::DecodeRawFrameHeader(malformedHeader));
	malformedHeader[4] = dev::ipc::RAW_FRAME_SCHEMA_VERSION;
	malformedHeader[5] = 99;
	ASSERT_TRUE(!dev::ipc::DecodeRawFrameHeader(malformedHeader));
	malformedHeader[5] = static_cast<uint8_t>(dev::ipc::RawFrameKind::FRAME);
	malformedHeader[6] = 1;
	ASSERT_TRUE(!dev::ipc::DecodeRawFrameHeader(malformedHeader));
}

static void test_stack_sample_words()
{
	auto hw = std::make_unique<dev::Hardware>("", "", true);
	constexpr int stackPointer = 0x1000;
	std::vector<uint8_t> bytes;
	for (int offset = -10; offset <= 10; offset += 2) {
		auto value = static_cast<uint16_t>(0x2000 + offset);
		bytes.push_back(static_cast<uint8_t>(value & 0xFF));
		bytes.push_back(static_cast<uint8_t>(value >> 8));
	}
	ASSERT_TRUE(hw->Request(dev::Hardware::Req::SET_MEM,
		{{"addr", stackPointer - 10}, {"data", bytes}}));

	for (bool running : {false, true}) {
		if (running) ASSERT_TRUE(hw->Request(dev::Hardware::Req::RUN));
		auto result = hw->Request(dev::Hardware::Req::GET_STACK_SAMPLE,
			{{"addr", stackPointer}});
		ASSERT_TRUE(result);
		if (!result) continue;
		auto sample = *result;
		for (int offset = -10; offset <= 10; offset += 2) {
			ASSERT_EQ(sample[std::to_string(offset)].get<uint16_t>(),
				static_cast<uint16_t>(0x2000 + offset));
		}
		if (running) ASSERT_TRUE(hw->Request(dev::Hardware::Req::STOP));
	}
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

static void test_hardware_statistics_and_mutations()
{
	auto hw = std::make_unique<dev::Hardware>("", "", true);

	auto stats = *hw->Request(dev::Hardware::Req::GET_HARDWARE_STATS);
	ASSERT_TRUE(stats["sessionId"].get<uint64_t>() > 0);
	ASSERT_EQ(stats["palette"].size(), (size_t)16);
	ASSERT_EQ(stats["fdc"]["drives"].size(), (size_t)4);
	ASSERT_EQ(stats["cpuCycles"].get<uint64_t>(), uint64_t(0));
	ASSERT_EQ(stats["lastRunCycles"].get<uint64_t>(), uint64_t(0));

	auto paletteResult = *hw->Request(dev::Hardware::Req::SET_IO_PALETTE_ENTRY,
		{{"index", 5}, {"hwColor", 0xFD}});
	ASSERT_EQ(paletteResult["index"].get<int>(), 5);
	ASSERT_EQ(paletteResult["hwColor"].get<int>(), 0xFD);
	stats = *hw->Request(dev::Hardware::Req::GET_HARDWARE_STATS);
	ASSERT_EQ(stats["palette"][5].get<int>(), 0xFD);

	std::vector<uint8_t> disk(FDD_SIZE, 0xA5);
	hw->Request(dev::Hardware::Req::MOUNT_FDD,
		{{"data", disk}, {"driveIdx", 0}, {"path", "stats.fdd"}, {"autoBoot", false}});
	auto dismountResult = *hw->Request(dev::Hardware::Req::DISMOUNT_FDD, {{"driveIdx", 0}});
	ASSERT_TRUE(!dismountResult["mounted"].get<bool>());
	hw->Request(dev::Hardware::Req::DISMOUNT_FDD, {{"driveIdx", 0}});
	stats = *hw->Request(dev::Hardware::Req::GET_HARDWARE_STATS);
	ASSERT_TRUE(!stats["fdc"]["drives"][0]["mounted"].get<bool>());
	ASSERT_EQ(stats["fdc"]["drives"][0]["path"].get<std::string>(), std::string(""));
	ASSERT_EQ(stats["fdc"]["selectedDrive"].get<int>(), 0);

	hw->Request(dev::Hardware::Req::RUN);
	std::this_thread::sleep_for(std::chrono::milliseconds(10));
	const auto runningStats = *hw->Request(dev::Hardware::Req::GET_HARDWARE_STATS);
	ASSERT_TRUE(runningStats["cpuCycles"].get<uint64_t>() > 0);
	ASSERT_EQ(runningStats["palette"].size(), (size_t)16);
	ASSERT_EQ(runningStats["fdc"]["drives"].size(), (size_t)4);
	bool paletteRejected = false;
	try {
		hw->Request(dev::Hardware::Req::SET_IO_PALETTE_ENTRY, {{"index", 0}, {"hwColor", 1}});
	} catch (const std::runtime_error&) {
		paletteRejected = true;
	}
	ASSERT_TRUE(paletteRejected);
	bool dismountRejected = false;
	try {
		hw->Request(dev::Hardware::Req::DISMOUNT_FDD, {{"driveIdx", 0}});
	} catch (const std::runtime_error&) {
		dismountRejected = true;
	}
	ASSERT_TRUE(dismountRejected);
	hw->Request(dev::Hardware::Req::STOP);
	const auto stoppedStats = *hw->Request(dev::Hardware::Req::GET_HARDWARE_STATS);
	const auto lastRunCycles = stoppedStats["lastRunCycles"].get<uint64_t>();
	ASSERT_TRUE(lastRunCycles > 0);
	const auto repeatedStats = *hw->Request(dev::Hardware::Req::GET_HARDWARE_STATS);
	ASSERT_EQ(repeatedStats["lastRunCycles"].get<uint64_t>(), lastRunCycles);

	const auto cyclesBeforeReset = repeatedStats["cpuCycles"].get<uint64_t>();
	const auto framesBeforeReset = repeatedStats["frameNumber"].get<uint64_t>();
	hw->Request(dev::Hardware::Req::RESET);
	const auto resetStats = *hw->Request(dev::Hardware::Req::GET_HARDWARE_STATS);
	ASSERT_EQ(resetStats["cpuCycles"].get<uint64_t>(), cyclesBeforeReset);
	ASSERT_EQ(resetStats["frameNumber"].get<uint64_t>(), framesBeforeReset);

	const auto oldSessionId = resetStats["sessionId"].get<uint64_t>();
	hw->BeginSession();
	const auto newSessionStats = *hw->Request(dev::Hardware::Req::GET_HARDWARE_STATS);
	ASSERT_TRUE(newSessionStats["sessionId"].get<uint64_t>() > oldSessionId);
	ASSERT_EQ(newSessionStats["cpuCycles"].get<uint64_t>(), uint64_t(0));
	ASSERT_EQ(newSessionStats["frameNumber"].get<uint64_t>(), uint64_t(0));
	ASSERT_EQ(newSessionStats["lastRunCycles"].get<uint64_t>(), uint64_t(0));
}

static void test_io_port_data_is_256_byte_binary()
{
	auto hw = std::make_unique<dev::Hardware>("", "", true);

	for (const auto command : {
		dev::Hardware::Req::GET_IO_PORTS_IN_DATA,
		dev::Hardware::Req::GET_IO_PORTS_OUT_DATA}) {
		const auto response = dev::ipc::MakeResponse(*hw->Request(command));
		const auto encoded = dev::ipc::Encode(response);
		const std::vector<uint8_t> payload(encoded.begin() + 4, encoded.end());
		const auto decoded = dev::ipc::Decode(payload);
		const auto& data = decoded[dev::ipc::FIELD_DATA];

		ASSERT_EQ(data.size(), (size_t)1);
		ASSERT_TRUE(data["bytes"].is_binary());
		const auto bytes = data["bytes"].get<nlohmann::json::binary_t>();
		ASSERT_EQ(bytes.size(), (size_t)256);
		ASSERT_TRUE(std::all_of(bytes.begin(), bytes.end(), [](uint8_t value) { return value == 0; }));
	}
}

int main()
{
	test_hardware_command_ids();
	test_protocol_encode_decode();
	test_response_helpers();
	test_request_validation();
	test_server_info();
	test_stop_record_lifecycle();
	test_structured_breakpoints();
	test_structured_watchpoints();
	test_watchpoint_matching();
	test_watchpoints_break_execution();
	test_breakpoint_stop_record();
	test_raw_frame_codec();
	test_stack_sample_words();
	test_ping_pong();
	test_get_regs();
	test_malformed_request_does_not_stop_emulation();
	test_memory_round_trip();
	test_get_mem_round_trip();
	test_run_and_fetch_frame();
	test_load_rom();
	test_mount_fdd();
	test_fdd_persistence();
	test_hardware_statistics_and_mutations();
	test_io_port_data_is_256_byte_binary();

	std::cout << "IPC Tests: " << tests_passed << "/" << tests_run << " passed";
	if (tests_failed > 0) {
		std::cout << " (" << tests_failed << " FAILED)";
	}
	std::cout << std::endl;
	return (tests_failed == 0) ? 0 : 1;
}
