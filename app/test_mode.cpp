#include "test_mode.h"

#include <iostream>
#include <format>
#include <vector>
#include <cstdint>

#include "core/hardware.h"
#include "core/memory.h"
#include "utils/utils.h"

int RunTestMode(dev::Hardware& _hw, const std::string& _romPath,
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
