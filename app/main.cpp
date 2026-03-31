#include <iostream>
#include <iostream>
#include <string>
#include <format>
#include <vector>
#include <memory>
#include <cstdint>

#include "core/hardware.h"
#include "utils/utils.h"
#include "utils/args_parser.h"

static constexpr uint8_t TEST_PORT = 0xED;

int main(int argc, char* argv[])
{
	dev::ArgsParser args(argc, argv, "v6emul - Vector-06C Emulator");

	auto romPath = args.GetString("rom", "Path to a ROM file to load", false, "");
	auto loadAddr = args.GetInt("load-addr", "ROM load address in hex (default: 0)", false, 0);
	auto runFrames = args.GetInt("run-frames", "Run for N frames then exit", false, 0);
	auto runCycles = args.GetInt("run-cycles", "Run for N CPU cycles then exit", false, 0);
	bool haltExit = args.HasFlag("halt-exit");
	bool dumpCpu = args.HasFlag("dump-cpu");
	bool dumpMemory = args.HasFlag("dump-memory");
	auto tcpPort = args.GetInt("tcp-port", "TCP port for IPC server (default: 9876)", false, 9876);
	auto logLevel = args.GetString("log-level", "Log verbosity: error, warn, info, debug, trace", false, "info");

	if (!args.IsRequirementSatisfied()) return 1;

	// If no ROM and no stop condition, just print banner and exit
	if (romPath.empty() && !haltExit && runFrames == 0 && runCycles == 0) {
		std::cout << "v6emul - Vector-06C Emulator" << std::endl;
		std::cout << "Use -rom <file> with -halt-exit, -run-frames, or -run-cycles to run a ROM." << std::endl;
		return 0;
	}

	// Need a stop condition
	if (!haltExit && runFrames == 0 && runCycles == 0) {
		std::cerr << "Error: specify at least one stop condition: -halt-exit, -run-frames N, or -run-cycles N" << std::endl;
		return 1;
	}

	// Load ROM file
	std::vector<uint8_t> romData;
	if (!romPath.empty()) {
		auto res = dev::LoadFile(romPath);
		if (!res) {
			std::cerr << std::format("Error: failed to load ROM file: {}", romPath) << std::endl;
			return 1;
		}
		romData = std::move(*res);
	}

	// Create hardware (heap-allocated due to ~2MB Memory inside)
	auto hw = std::make_unique<dev::Hardware>("", "", true);

	// Set up test port output callback
	hw->SetDebugPortOutCallback([](uint8_t port, uint8_t value) {
		std::cout << std::format("TEST_OUT port=0x{:02X} value=0x{:02X}", port, value) << std::endl;
	});

	// Load ROM data into RAM
	if (!romData.empty()) {
		nlohmann::json setMemJ = {
			{"addr", loadAddr},
			{"data", romData}
		};
		hw->Request(dev::Hardware::Req::SET_MEM, setMemJ);
	}

	// Restart: disable ROM mapping, reset CPU to PC=0
	hw->Request(dev::Hardware::Req::RESTART);

	// Run headless with stop conditions
	nlohmann::json headlessJ = {
		{"haltExit", haltExit},
		{"maxFrames", static_cast<uint64_t>(runFrames)},
		{"maxCycles", static_cast<uint64_t>(runCycles)}
	};
	auto result = hw->Request(dev::Hardware::Req::RUN_HEADLESS, headlessJ);

	if (!result) {
		std::cerr << "Error: headless run failed" << std::endl;
		return 1;
	}

	auto resJ = *result;
	auto pc = resJ["pc"].get<uint16_t>();
	auto cc = resJ["cc"].get<uint64_t>();
	auto frames = resJ["frames"].get<uint64_t>();
	auto halted = resJ["halted"].get<bool>();

	// Print halt/exit line
	if (halted) {
		std::cout << std::format("HALT at PC=0x{:04X} after {} cpu_cycles {} frames", pc, cc, frames) << std::endl;
	} else {
		std::cout << std::format("EXIT at PC=0x{:04X} after {} cpu_cycles {} frames", pc, cc, frames) << std::endl;
	}

	// Dump CPU state if requested
	if (dumpCpu) {
		auto af = resJ["af"].get<uint16_t>();
		auto bc = resJ["bc"].get<uint16_t>();
		auto de = resJ["de"].get<uint16_t>();
		auto hl = resJ["hl"].get<uint16_t>();
		auto sp = resJ["sp"].get<uint16_t>();

		std::cout << std::format("CPU: A={:02X} F={:02X} B={:02X} C={:02X} D={:02X} E={:02X} H={:02X} L={:02X}",
			af >> 8, af & 0xFF, bc >> 8, bc & 0xFF, de >> 8, de & 0xFF, hl >> 8, hl & 0xFF) << std::endl;
		std::cout << std::format("     PC={:04X} SP={:04X} CC={}", pc, sp, cc) << std::endl;
	}

	// Dump memory if requested
	if (dumpMemory) {
		auto ram = hw->GetRam();
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

	return 0;
}
