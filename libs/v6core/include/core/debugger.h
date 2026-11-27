#pragma once

#include <memory>
#include <chrono>
#include <unordered_map>
#include <vector>
#include <array>
#include <format>

#include "utils/types.h"
#include "core/hardware.h"
#include "core/disasm.h"
#include "core/debug_data.h"
#include "core/display.h"
#include "core/breakpoint.h"
#include "core/watchpoint.h"
#include "core/trace_log.h"

namespace dev
{
	class Debugger
	{
	public:
		explicit Debugger(Hardware& _hardware);
		~Debugger();

		void Reset();

		bool Debug(CpuI8080::State* _cpuStateP, Memory::State* _memStateP,
			IO::State* _ioStateP, Display::State* _displayStateP);
		auto DebugReqHandling(Hardware::Req _req, nlohmann::json _reqDataJ,
			CpuI8080::State* _cpuStateP, Memory::State* _memStateP,
			IO::State* _ioStateP, Display::State* _displayStateP) -> nlohmann::json;

		auto GetTraceLog() -> TraceLog& { return m_traceLog; };
		auto GetDebugData() -> DebugData& { return m_debugData; };
		auto GetDisasm() -> Disasm& { return m_disasm; };

	private:

		Hardware& m_hardware;
		DebugData m_debugData;
		Disasm m_disasm;
		TraceLog m_traceLog;

		// Owned entirely by the hardware thread, including IPC snapshots.
		struct RecentAccess {
			std::array<GlobalAddr, 1024> addresses{};
			size_t next = 0;
			size_t count = 0;
			void Clear() { next = count = 0; }
			void Add(GlobalAddr address);
			auto Intensities(size_t base) const -> nlohmann::json;
		};
		RecentAccess m_recentReads, m_recentWrites;
		bool m_collectMemoryDisplay = false;
		unsigned m_memoryDisplayLeaseCheck = 0;
		std::chrono::steady_clock::time_point m_memoryDisplayDeadline;
		void RefreshMemoryDisplayLease(bool enabled);
	};
}