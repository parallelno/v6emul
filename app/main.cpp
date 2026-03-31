#include <iostream>
#include <string>
#include <format>
#include <vector>
#include <memory>
#include <cstdint>

#include "core/hardware.h"
#include "core/memory.h"
#include "utils/utils.h"
#include "utils/args_parser.h"
#include "ipc/transport.h"
#include "ipc/protocol.h"
#include "ipc/commands.h"

static constexpr uint8_t TEST_PORT = 0xED;

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
	}

	// Restart: disable ROM mapping, reset CPU to PC=0
	_hw.Request(dev::Hardware::Req::RESTART);

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
static int RunServerMode(dev::Hardware& _hw, uint16_t _port)
{
	dev::ipc::Transport server;

	if (!server.Listen(_port)) {
		std::cerr << std::format("Error: failed to listen on port {}", _port) << std::endl;
		return 1;
	}

	std::cout << std::format("IPC server listening on 127.0.0.1:{}", server.GetPort()) << std::endl;

	// Start emulation in RUN state
	_hw.Request(dev::Hardware::Req::RUN);

	while (true) {
		if (!server.IsClientConnected()) {
			std::cout << "Waiting for client..." << std::endl;
			if (!server.AcceptClient()) {
				std::cerr << "Error: failed to accept client" << std::endl;
				continue;
			}
			std::cout << "Client connected" << std::endl;
		}

		// Receive one message
		auto payload = server.Recv();
		if (payload.empty()) {
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
	return 0;
}

// ── Entry point ──────────────────────────────────────────────────────
int main(int argc, char* argv[])
{
	dev::ArgsParser args(argc, argv, "v6emul - Vector-06C Emulator");

	// Check for --version early
	if (args.HasFlag("version") || args.HasFlag("V")) {
		std::cout << "v6emul 0.1.0" << std::endl;
		return 0;
	}

	auto romPath = args.GetString("rom", "Path to a ROM file to load", false, "");
	auto loadAddr = args.GetInt("load-addr", "ROM load address in hex (default: 0)", false, 0);
	auto runFrames = args.GetInt("run-frames", "Run for N frames then exit", false, 0);
	auto runCycles = args.GetInt("run-cycles", "Run for N CPU cycles then exit", false, 0);
	bool haltExit = args.HasFlag("halt-exit");
	bool dumpCpu = args.HasFlag("dump-cpu");
	bool dumpMemory = args.HasFlag("dump-memory");
	auto dumpRamdisk = args.GetInt("dump-ramdisk", "Print RAM-disk N (0-7) contents on exit", false, -1);
	bool serve = args.HasFlag("serve");
	auto tcpPort = args.GetInt("tcp-port", "TCP port for IPC server (default: 9876)", false, 9876);
	auto speed = args.GetString("speed", "Execution speed: 1%, 20%, 50%, 100%, 200%, max", false, "");
	auto logLevel = args.GetString("log-level", "Log verbosity: error, warn, info, debug, trace", false, "info");

	if (!args.IsRequirementSatisfied()) return 1;

	bool testMode = haltExit || runFrames > 0 || runCycles > 0;

	// Banner mode: no flags at all
	if (!testMode && !serve) {
		std::cout << "v6emul - Vector-06C Emulator" << std::endl;
		std::cout << "Use --serve to start the IPC server, or" << std::endl;
		std::cout << "    --rom <file> with --halt-exit, --run-frames, or --run-cycles for test mode." << std::endl;
		return 0;
	}

	// Create hardware (heap-allocated due to ~2MB Memory inside)
	auto hw = std::make_unique<dev::Hardware>("", "", true);

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

	if (testMode) {
		return RunTestMode(*hw, romPath, loadAddr, haltExit, runFrames, runCycles, dumpCpu, dumpMemory, dumpRamdisk);
	}

	// Server mode
	return RunServerMode(*hw, static_cast<uint16_t>(tcpPort));
}
