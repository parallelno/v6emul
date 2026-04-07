#pragma once

#include <string>

namespace dev { class Hardware; }

int RunTestMode(dev::Hardware& hw, const std::string& romPath,
	int loadAddr, bool haltExit, int runFrames, int runCycles,
	bool dumpCpu, bool dumpMemory, int dumpRamdisk);
