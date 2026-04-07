#include <iostream>
#include <string>
#include <format>
#include <vector>
#include <memory>
#include <cstdint>

#include "core/hardware.h"
#include "core/fdd_consts.h"
#include "core/display.h"
#include "utils/utils.h"
#include "utils/args_parser.h"
#include "v6emul_version.h"

#include "server_mode.h"
#include "test_mode.h"

namespace
{
	constexpr const char* APP_TITLE = "v6emul - Vector-06C Emulator";
	constexpr const char* APP_COPYRIGHT = "(c) Aleksandr Fedotovskikh <mailforfriend@gmail.com>";

	#define APP_HEADER \
		"Command-line emulator for the Vector-06C Soviet PC, version " \
		V6EMUL_VERSION "\n" \
		"(c) Aleksandr Fedotovskikh <mailforfriend@gmail.com>"

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

// ── Entry point ──────────────────────────────────────────────────────
int main(int argc, char* argv[])
{
	dev::ArgsParser args(argc, argv, APP_HEADER);

	// Check for --version early
	if (args.HasFlag("version", "Print version and exit") || args.HasFlag("V")) {
		std::cout << V6EMUL_VERSION << "\n\n";
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
	auto colorFormat = args.GetString("color-format", "Pixel format for GET_FRAME_RAW: abgr (default), argb", false, "abgr");
	auto frameModeStr = args.GetString("frame-mode", "Frame region returned by IPC: full, bordered (default), borderless", false, "bordered");
	auto logLevel = args.GetString("log-level", "Log verbosity: error, warn, info, debug, trace", false, "info");

	if (!args.IsRequirementSatisfied()) return 1;
	if (!args.CheckUnknownArgs()) return 1;
	if (args.HasFlag("help") || args.HasFlag("h")) return 0;

	bool testMode = haltExit || runFrames > 0 || runCycles > 0;

	// Banner mode: no flags at all
	if (!testMode && !serve) {
		std::cout << std::format(
			"{}\nUse --serve to start the IPC server, or\n    --rom <file> with --halt-exit, --run-frames, or --run-cycles for test mode.",
			APP_TITLE) << std::endl << std::endl;
		return 0;
	}

	auto resolvedBootPath = dev::ResolvePath(bootRomPath);
	if (!bootRomPath.empty() && !dev::IsFileExist(resolvedBootPath)) {
		std::cerr << std::format("Error: boot ROM file does not exist: {}, {}", bootRomPath, resolvedBootPath) << std::endl;
		return 1;
	}

	if (fddDrive < 0 || fddDrive > 3) {
		std::cerr << std::format("Error: --fdd-drive must be 0-3, got {}", fddDrive) << std::endl;
		return 1;
	}

	dev::Display::ColorFormat colorFormatEnum = dev::Display::ColorFormat::ABGR;
	if (colorFormat == "argb") {
		colorFormatEnum = dev::Display::ColorFormat::ARGB;
	} else if (colorFormat != "abgr") {
		std::cerr << std::format("Error: --color-format must be 'abgr' or 'argb', got '{}'", colorFormat) << std::endl;
		return 1;
	}

	dev::Display::FrameMode frameMode = dev::Display::FrameMode::BORDERED;
	if (frameModeStr == "full") {
		frameMode = dev::Display::FrameMode::FULL;
	} else if (frameModeStr == "bordered") {
		frameMode = dev::Display::FrameMode::BORDERED;
	} else if (frameModeStr == "borderless") {
		frameMode = dev::Display::FrameMode::BORDERLESS;
	} else {
		std::cerr << std::format("Error: --frame-mode must be 'full', 'bordered', or 'borderless', got '{}'", frameModeStr) << std::endl;
		return 1;
	}

	// Create hardware (heap-allocated due to ~2MB Memory inside)
	auto hw = std::make_unique<dev::Hardware>(bootRomPath, "", true);

	// Apply frame mode
	hw->Request(dev::Hardware::Req::SET_FRAME_MODE,
		{{"frameMode", static_cast<int>(frameMode)}});

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
	return RunServerMode(*hw, static_cast<uint16_t>(tcpPort), colorFormatEnum);
}
