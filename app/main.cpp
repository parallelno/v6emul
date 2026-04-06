#include <iostream>
#include <string>
#include <format>
#include <vector>
#include <memory>
#include <cstdint>
#include <atomic>
#include <csignal>

#include "core/hardware.h"
#include "core/memory.h"
#include "core/fdd_consts.h"
#include "utils/utils.h"
#include "utils/args_parser.h"
#include "ipc/transport.h"
#include "ipc/protocol.h"
#include "ipc/commands.h"
#include "v6emul_version.h"

static constexpr uint8_t TEST_PORT = 0xED;

namespace
{
	constexpr const char* APP_TITLE = "v6emul - Vector-06C Emulator";
	constexpr const char* APP_COPYRIGHT = "(c) Aleksandr Fedotovskikh <mailforfriend@gmail.com>";

	auto GetVersionString() -> std::string
	{
		return V6EMUL_VERSION;
	}

	auto GetCliDescription() -> std::string
	{
		return std::format(
			"Command-line emulator for the Vector-06C Soviet PC, version {}\n{}",
			GetVersionString(),
			APP_COPYRIGHT);
	}

	void PrintTextBlock(const std::string& text)
	{
		std::cout << text << "\n\n";
	}

	auto CanLoadStartupFile(const std::string& path) -> bool
	{
		if (path.empty()) {
			return true;
		}

		if (dev::LoadFile(path)) {
			return true;
		}

		return static_cast<bool>(dev::LoadFile(dev::GetExecutableDir() + path));
	}
}

// ── Test mode: load ROM, run headless, print results ─────────────────
static int RunTestMode(dev::Hardware& _hw, const std::string& _romPath,
	int _loadAddr, bool _haltExit, int _runFrames, int _runCycles,
	bool _dumpCpu, bool _dumpMemory, int _dumpRamdisk)
{
	// Load ROM file
	std::vector<uint8_t> romData;
	if (!_romPath.empty()) {
		auto res = dev::LoadFile(_romPath);
		if (!res) {
			std::cerr << std::format("Error: failed to load ROM file: {}", _romPath) << std::endl;
			return 1;
		}
		romData = std::move(*res);
	}

	// Set up test port output callback
	_hw.SetDebugPortOutCallback([](uint8_t port, uint8_t value) {
		std::cout << std::format("TEST_OUT port=0x{:02X} value=0x{:02X}", port, value) << std::endl;
	});

	// Load ROM data into RAM
	if (!romData.empty()) {
		nlohmann::json setMemJ = {
			{"addr", _loadAddr},
			{"data", romData}
		};
		_hw.Request(dev::Hardware::Req::SET_MEM, setMemJ);

		// Restart into RAM execution after loading a test ROM image.
		_hw.Request(dev::Hardware::Req::RESTART);
	}

	// Run headless with stop conditions
	nlohmann::json headlessJ = {
		{"haltExit", _haltExit},
		{"maxFrames", static_cast<uint64_t>(_runFrames)},
		{"maxCycles", static_cast<uint64_t>(_runCycles)}
	};
	auto result = _hw.Request(dev::Hardware::Req::RUN_HEADLESS, headlessJ);

	if (!result) {
		std::cerr << "Error: headless run failed" << std::endl;
		return 1;
	}

	auto resJ = *result;
	auto pc = resJ["pc"].get<uint16_t>();
	auto cc = resJ["cc"].get<uint64_t>();
	auto frames = resJ["frames"].get<uint64_t>();
	auto halted = resJ["halted"].get<bool>();

	if (halted) {
		std::cout << std::format("HALT at PC=0x{:04X} after {} cpu_cycles {} frames", pc, cc, frames) << std::endl;
	} else {
		std::cout << std::format("EXIT at PC=0x{:04X} after {} cpu_cycles {} frames", pc, cc, frames) << std::endl;
	}

	if (_dumpCpu) {
		auto af = resJ["af"].get<uint16_t>();
		auto bc = resJ["bc"].get<uint16_t>();
		auto de = resJ["de"].get<uint16_t>();
		auto hl = resJ["hl"].get<uint16_t>();
		auto sp = resJ["sp"].get<uint16_t>();

		std::cout << std::format("CPU: A={:02X} F={:02X} B={:02X} C={:02X} D={:02X} E={:02X} H={:02X} L={:02X}",
			af >> 8, af & 0xFF, bc >> 8, bc & 0xFF, de >> 8, de & 0xFF, hl >> 8, hl & 0xFF) << std::endl;
		std::cout << std::format("     PC={:04X} SP={:04X} CC={}", pc, sp, cc) << std::endl;
	}

	if (_dumpMemory) {
		auto ram = _hw.GetRam();
		if (ram) {
			std::cout << "MEMORY:" << std::endl;
			for (int addr = 0; addr < 0x10000; addr += 16) {
				std::cout << std::format("{:04X}:", addr);
				for (int j = 0; j < 16; j++) {
					std::cout << std::format(" {:02X}", (*ram)[addr + j]);
				}
				std::cout << std::endl;
			}
		}
	}

	if (_dumpRamdisk >= 0 && _dumpRamdisk < static_cast<int>(dev::Memory::RAM_DISK_MAX)) {
		auto ram = _hw.GetRam();
		if (ram) {
			size_t base = dev::Memory::MEMORY_MAIN_LEN
				+ static_cast<size_t>(_dumpRamdisk) * dev::Memory::MEMORY_RAMDISK_LEN;
			std::cout << std::format("RAMDISK {}:", _dumpRamdisk) << std::endl;
			for (size_t offset = 0; offset < dev::Memory::MEMORY_RAMDISK_LEN; offset += 16) {
				std::cout << std::format("{:06X}:", offset);
				for (int j = 0; j < 16; j++) {
					std::cout << std::format(" {:02X}", (*ram)[base + offset + j]);
				}
				std::cout << std::endl;
			}
		}
	}

	return 0;
}

// ── IPC server mode: TCP recv loop → Hardware::Request() → response ──
static std::atomic<bool> g_shutdown{false};
static dev::ipc::Transport* g_serverPtr = nullptr;

static void SignalHandler(int)
{
	g_shutdown.store(true);
	// Close the server sockets to unblock accept()/recv()
	if (g_serverPtr) g_serverPtr->Close();
}

static int RunServerMode(dev::Hardware& _hw, uint16_t _port, bool _convertBgra)
{
	std::signal(SIGINT, SignalHandler);
	std::signal(SIGTERM, SignalHandler);

	dev::ipc::Transport server;
	g_serverPtr = &server;

	if (!server.Listen(_port)) {
		std::cerr << std::format("Error: failed to listen on port {}", _port) << std::endl;
		return 1;
	}

	std::cout << std::format("IPC server listening on 127.0.0.1:{} (Ctrl+C to stop)", server.GetPort()) << std::endl;

	// Start emulation in RUN state
	_hw.Request(dev::Hardware::Req::RUN);

	while (!g_shutdown.load()) {
		if (!server.IsClientConnected()) {
			std::cout << "Waiting for client..." << std::endl;
			if (!server.AcceptClient()) {
				if (g_shutdown.load()) break;
				std::cerr << "Error: failed to accept client" << std::endl;
				continue;
			}
			std::cout << "Client connected" << std::endl;
		}

		// Receive one message
		auto payload = server.Recv();
		if (payload.empty()) {
			if (g_shutdown.load()) break;
			std::cout << "Client disconnected" << std::endl;
			server.Close();
			// Re-listen for next client
			if (!server.Listen(_port)) {
				std::cerr << "Error: failed to re-listen" << std::endl;
				return 1;
			}
			continue;
		}

		// Decode request
		nlohmann::json requestJ;
		try {
			requestJ = dev::ipc::Decode(payload);
		} catch (const std::exception& e) {
			auto errResp = dev::ipc::Encode(
				dev::ipc::MakeErrorResponse(std::format("decode error: {}", e.what())));
			server.Send(errResp);
			continue;
		}

		int cmdInt = requestJ.value(dev::ipc::FIELD_CMD, 0);
		auto dataJ = requestJ.value(dev::ipc::FIELD_DATA, nlohmann::json{});

		// Handle pseudo-commands
		if (cmdInt == dev::ipc::CMD_PING) {
			auto resp = dev::ipc::Encode(dev::ipc::MakeResponse({{"pong", true}}));
			server.Send(resp);
			continue;
		}

		if (cmdInt == dev::ipc::CMD_GET_FRAME) {
			auto* fb = _hw.GetFrame(false);
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
			auto respBytes = dev::ipc::Encode(responseJ);
			server.Send(respBytes);
			continue;
		}

		// Raw binary frame: [4:payloadLen][4:width][4:height][raw ABGR pixels]
		// Bypasses json/msgpack for high-throughput frame streaming.
		if (cmdInt == dev::ipc::CMD_GET_FRAME_RAW) {
			auto* fb = _hw.GetFrame(false);
			if (fb) {
				size_t pixLen = fb->size() * sizeof(dev::ColorI);
				uint32_t payloadLen = static_cast<uint32_t>(8 + pixLen);
				uint32_t w = dev::Display::FRAME_W;
				uint32_t h = dev::Display::FRAME_H;

				// Reuse a static buffer to avoid per-frame allocation
				static std::vector<uint8_t> rawMsg;
				rawMsg.resize(4 + 8 + pixLen);
				std::memcpy(rawMsg.data(), &payloadLen, 4);
				std::memcpy(rawMsg.data() + 4, &w, 4);
				std::memcpy(rawMsg.data() + 8, &h, 4);
				std::memcpy(rawMsg.data() + 12, fb->data(), pixLen);

				// Convert ABGR (ImGui) → ARGB (BGRA byte order) if requested
				if (_convertBgra) {
					auto* pixels = reinterpret_cast<uint32_t*>(rawMsg.data() + 12);
					size_t pixelCount = fb->size();
					for (size_t i = 0; i < pixelCount; ++i) {
						uint32_t c = pixels[i];
						pixels[i] = (c & 0xFF00FF00u) | ((c & 0xFFu) << 16) | ((c >> 16) & 0xFFu);
					}
				}

				server.Send(rawMsg);
			} else {
				auto errResp = dev::ipc::Encode(
					dev::ipc::MakeErrorResponse("no frame available"));
				server.Send(errResp);
			}
			continue;
		}

		// Dispatch to Hardware::Request()
		auto req = static_cast<dev::Hardware::Req>(cmdInt);

		// EXIT command: respond, then shut down
		if (req == dev::Hardware::Req::EXIT) {
			auto resp = dev::ipc::Encode(dev::ipc::MakeResponse({{"exiting", true}}));
			server.Send(resp);
			_hw.Request(dev::Hardware::Req::EXIT);
			break;
		}

		auto result = _hw.Request(req, dataJ);

		nlohmann::json responseJ;
		if (result) {
			responseJ = dev::ipc::MakeResponse(*result);
		} else {
			responseJ = dev::ipc::MakeErrorResponse("request failed");
		}

		auto respBytes = dev::ipc::Encode(responseJ);
		if (!server.Send(respBytes)) {
			std::cout << "Client disconnected during send" << std::endl;
			server.Close();
			if (!server.Listen(_port)) {
				std::cerr << "Error: failed to re-listen" << std::endl;
				return 1;
			}
		}
	}

	server.Close();
	g_serverPtr = nullptr;
	if (g_shutdown.load()) {
		std::cout << "\nShutting down..." << std::endl;
		_hw.Request(dev::Hardware::Req::EXIT);
	}
	return 0;
}

// ── Entry point ──────────────────────────────────────────────────────
int main(int argc, char* argv[])
{
	dev::ArgsParser args(argc, argv, GetCliDescription());

	// Check for --version early
	if (args.HasFlag("version", "Print version and exit") || args.HasFlag("V")) {
		PrintTextBlock(GetVersionString());
		return 0;
	}

	auto romPath = args.GetString("rom", "Path to a ROM file to load", false, "");
	auto bootRomPath = args.GetString("boot-rom", "Path to a boot ROM file to map at address 0 on startup/reset", false, "");
	auto loadAddr = args.GetInt("load-addr", "ROM load address in hex (default: 0)", false, 0);
	auto fddPath = args.GetString("fdd", "Path to a floppy disk image to mount", false, "");
	auto fddDrive = args.GetInt("fdd-drive", "FDD drive index (0-3, default: 0)", false, 0);
	bool fddAutoboot = args.HasFlag("fdd-autoboot", "Reset and boot from the mounted floppy disk");
	auto runFrames = args.GetInt("run-frames", "Run for N frames then exit", false, 0);
	auto runCycles = args.GetInt("run-cycles", "Run for N CPU cycles then exit", false, 0);
	bool haltExit = args.HasFlag("halt-exit", "Stop execution when CPU executes HLT");
	bool dumpCpu = args.HasFlag("dump-cpu", "Print CPU registers on exit");
	bool dumpMemory = args.HasFlag("dump-memory", "Print full 64KB RAM dump on exit");
	auto dumpRamdisk = args.GetInt("dump-ramdisk", "Print RAM-disk N (0-7) contents on exit", false, -1);
	bool serve = args.HasFlag("serve", "Start the IPC server mode");
	auto tcpPort = args.GetInt("tcp-port", "TCP port for IPC server (default: 9876)", false, 9876);
	auto speed = args.GetString("speed", "Execution speed: 1%, 20%, 50%, 100%, 200%, max", false, "");
	auto frameFormat = args.GetString("frame-format", "Pixel format for GET_FRAME_RAW: rgba (default), bgra", false, "rgba");
	auto logLevel = args.GetString("log-level", "Log verbosity: error, warn, info, debug, trace", false, "info");

	if (!args.IsRequirementSatisfied()) return 1;
	if (!args.CheckUnknownArgs()) return 1;
	if (args.HasFlag("help") || args.HasFlag("h")) return 0;

	bool testMode = haltExit || runFrames > 0 || runCycles > 0;

	// Banner mode: no flags at all
	if (!testMode && !serve) {
		PrintTextBlock(
			std::format(
				"{}\nUse --serve to start the IPC server, or\n    --rom <file> with --halt-exit, --run-frames, or --run-cycles for test mode.",
				APP_TITLE));
		return 0;
	}

	if (!bootRomPath.empty() && !CanLoadStartupFile(bootRomPath)) {
		std::cerr << std::format("Error: failed to load boot ROM file: {}", bootRomPath) << std::endl;
		return 1;
	}

	if (fddDrive < 0 || fddDrive > 3) {
		std::cerr << std::format("Error: --fdd-drive must be 0-3, got {}", fddDrive) << std::endl;
		return 1;
	}

	bool convertBgra = false;
	if (frameFormat == "bgra") {
		convertBgra = true;
	} else if (frameFormat != "rgba") {
		std::cerr << std::format("Error: --frame-format must be 'rgba' or 'bgra', got '{}'", frameFormat) << std::endl;
		return 1;
	}

	// Create hardware (heap-allocated due to ~2MB Memory inside)
	auto hw = std::make_unique<dev::Hardware>(bootRomPath, "", true);

	// Apply speed setting if provided
	if (!speed.empty()) {
		int speedIdx = -1;
		if (speed == "1%")   speedIdx = static_cast<int>(dev::Hardware::ExecSpeed::_1PERCENT);
		else if (speed == "20%")  speedIdx = static_cast<int>(dev::Hardware::ExecSpeed::_20PERCENT);
		else if (speed == "50%")  speedIdx = static_cast<int>(dev::Hardware::ExecSpeed::HALF);
		else if (speed == "100%") speedIdx = static_cast<int>(dev::Hardware::ExecSpeed::NORMAL);
		else if (speed == "200%") speedIdx = static_cast<int>(dev::Hardware::ExecSpeed::X2);
		else if (speed == "max")  speedIdx = static_cast<int>(dev::Hardware::ExecSpeed::MAX);
		if (speedIdx >= 0) {
			hw->Request(dev::Hardware::Req::SET_CPU_SPEED, {{"speed", speedIdx}});
		}
	}

	// Mount FDD if provided
	if (!fddPath.empty()) {
		auto fddData = dev::LoadFile(fddPath);
		if (!fddData) {
			std::cerr << std::format("Error: failed to load FDD image: {}", fddPath) << std::endl;
			return 1;
		}
		std::cout << std::format("Mounting FDD: {} ({} bytes) on drive {}", fddPath, fddData->size(), fddDrive) << std::endl;
		fddData->resize(FDD_SIZE, 0);
		nlohmann::json mountJ = {
			{"driveIdx", fddDrive},
			{"data", *fddData},
			{"path", fddPath}
		};
		hw->Request(dev::Hardware::Req::LOAD_FDD, mountJ);
		if (fddAutoboot) {
			hw->Request(dev::Hardware::Req::RESET);
		}
	}

	if (testMode) {
		return RunTestMode(*hw, romPath, loadAddr, haltExit, runFrames, runCycles, dumpCpu, dumpMemory, dumpRamdisk);
	}

	// Server mode: load ROM if provided
	if (!romPath.empty()) {
		auto res = dev::LoadFile(romPath);
		if (!res) {
			std::cerr << std::format("Error: failed to load ROM file: {}", romPath) << std::endl;
			return 1;
		}
		std::cout << std::format("Loading ROM: {} ({} bytes) at 0x{:04X}", romPath, res->size(), loadAddr) << std::endl;
		nlohmann::json setMemJ = {
			{"addr", loadAddr},
			{"data", *res}
		};
		hw->Request(dev::Hardware::Req::SET_MEM, setMemJ);
		hw->Request(dev::Hardware::Req::RESTART);
	}

	// Server mode
	return RunServerMode(*hw, static_cast<uint16_t>(tcpPort), convertBgra);
}
