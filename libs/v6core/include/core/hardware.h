#pragma once

#include <mutex>
#include <string>
#include <thread>
#include <condition_variable>
#include <atomic>
#include <chrono>
#include <exception>

#include "utils/types.h"
#include "core/cpu_i8080.h"
#include "core/memory.h"
#include "core/keyboard.h"
#include "core/io.h"
#include "core/display.h"
#include "core/timer_i8253.h"
#include "core/sound_ay8910.h"
#include "core/audio.h"
#include "core/fdc_wd1793.h"
#include "utils/utils.h"
#include "utils/result.h"
#include "utils/tqueue.h"
#include "utils/json_utils.h"

namespace dev
{
	class Hardware
	{
		CpuI8080 m_cpu;
		Memory m_memory;
		Keyboard m_keyboard;
		IO m_io;
		Display m_display;
		TimerI8253 m_timer;
		SoundAY8910 m_ay;
		AYWrapper m_aywrapper;
		Audio m_audio;
		Fdc1793 m_fdc;

		enum class Status : int {
			RUN,
			STOP,
			EXIT
		};

	public:
		//enum class Req
		#include "core/hardware_consts.h"


		using DebugFunc = std::function<bool(
			CpuI8080::State* _cpuState, Memory::State* _memState,
			IO::State* _ioState, Display::State* _displayState)>;

		using DebugReqHandlingFunc = std::function<nlohmann::json(
			Req _req, nlohmann::json _reqDataJ,
			CpuI8080::State* _cpuState, Memory::State* _memState,
			IO::State* _ioState, Display::State* _displayState)>;

		enum class ExecSpeed : int { _1PERCENT = 0,
									_20PERCENT,
									HALF,
									NORMAL,
									X2,
									MAX,
									LEN
								};

        Hardware(const std::string& _pathBootData,
			const std::string& _pathRamDiskData,
			const bool _ramDiskClearAfterRestart);
		~Hardware();
		void BeginSession();
		auto Request(
			const Req _req, const nlohmann::json& _dataJ = {})
			-> Result <nlohmann::json>;

		auto GetFrame(const bool _vsync)
			-> std::pair<const dev::Display::FrameBuffer*, dev::Display::FrameModeRegion>;
		auto GetRam() const -> const Memory::Ram*;
		auto GetCpuState() -> const CpuI8080::State& { return m_cpu.GetState(); }
		auto GetMemState() -> const Memory::State& { return m_memory.GetState(); }
		auto GetIoState() -> const IO::State& { return m_io.GetState(); }
		auto GetDisplayState() -> const Display::State& { return m_display.GetState(); }

		void AttachDebugFuncs(
			DebugFunc _debugFunc, DebugReqHandlingFunc _debugReqHandlingFunc);

		void SetDebugPortOutCallback(IO::DebugPortOutFunc _func);
		void RecordStop(const std::string& _reason,
			const nlohmann::json& _trigger = nlohmann::json::object());


	private:
		DebugFunc Debug = nullptr;
		DebugReqHandlingFunc DebugReqHandling = nullptr;
		bool m_debugAttached = false;

		std::thread m_executionThread;
		std::thread m_reqHandlingThread;
		std::atomic<Status> m_status;
		uint64_t m_sessionId = 0;
		uint64_t m_cycleOffset = 0;
		uint64_t m_frameOffset = 0;
		uint64_t m_sessionStartCycles = 0;
		uint64_t m_sessionStartFrames = 0;
		uint64_t m_runStartCycles = 0;
		uint64_t m_lastRunCycles = 0;
		std::chrono::steady_clock::time_point m_sessionStartTime;
		uint64_t m_stopSequence = 0;
		nlohmann::json m_stopRecord;
		struct RequestResult {
			nlohmann::json data;
			std::exception_ptr error;
			bool handled = true;
		};
		TQueue <std::pair<Req, nlohmann::json>> m_reqs; // request
		TQueue <RequestResult> m_reqRes;				// request's result sent back

		static constexpr std::chrono::microseconds m_reqHandlingTime = 1ms;
		ExecSpeed m_execSpeed = ExecSpeed::NORMAL;
		double m_actualSpeedPercent = 100.0;
		std::chrono::microseconds m_execDelays[static_cast<int>(ExecSpeed::LEN)] = {
			1996800us/*1%*/,
			99840us/*20%*/,
			39936us/*HALF*/,
			19968us/*NORMAL*/,
			9984us/*X2*/,
			0us/*MAX*/
		};

		void Init();
		auto GetTotalCycles() const -> uint64_t;
		auto GetTotalFrames() const -> uint64_t;
		void Execution();
		bool ExecuteInstruction();
		void ExecuteFrameNoBreaks();
		void ReqHandling(
			const std::chrono::duration<int64_t, std::nano> _waitTime = -1ns);
		auto RunHeadless(const nlohmann::json& _dataJ) -> nlohmann::json;
		void Reset();
		void Restart();
		void Stop(bool _record = true);
		void Run();
		auto GetRegs() const -> nlohmann::json;
		auto GetByteGlobal(const nlohmann::json _globalAddrJ) -> nlohmann::json;
		auto GetMem(const nlohmann::json _dataJ) -> nlohmann::json;

		auto GetByte(const nlohmann::json _addrJ,
					const Memory::AddrSpace _addrSpace) -> nlohmann::json;

		auto Get3Bytes(const nlohmann::json _addrJ,
					const Memory::AddrSpace _addrSpace) -> nlohmann::json;

		auto GetMemString(const nlohmann::json _dataJ) -> nlohmann::json;

		auto GetWord(const nlohmann::json _addrJ,
					const Memory::AddrSpace _addrSpace) -> nlohmann::json;

		auto GetStackSample(const nlohmann::json _addrJ) -> nlohmann::json;
		auto GetFddInfo(const int _driveIdx) -> Fdc1793::DiskInfo;
		auto GetFddImage(const int _driveIdx) -> const std::vector<uint8_t>;
		auto GetStepOverAddr() -> const Addr;
	};
}