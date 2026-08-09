#pragma once

#include <chrono>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <unordered_map>

#include <lua.hpp>
#include <nlohmann/json.hpp>

#include "core/cpu_i8080.h"
#include "core/display.h"
#include "core/io.h"
#include "core/memory.h"
#include "core/script.h"
#include "utils/types.h"

namespace dev
{
	struct ScriptsTestAccess;

	class Scripts
	{
	public:
		static constexpr size_t MAX_NAME_BYTES = 1024;
		static constexpr size_t MAX_PATH_BYTES = 4096;
		static constexpr size_t MAX_SOURCE_BYTES = 1024 * 1024;
		static constexpr size_t MAX_RECORDS = 256;
		static constexpr size_t MAX_ERROR_BYTES = 4096;
		static constexpr int MAX_INSTRUCTIONS_PER_RUN = 100000;
		static constexpr int MAX_EXECUTION_MILLISECONDS = 25;

		enum UIType { NONE = 0, TEXT, RECT, RECT_FILLED };

		struct UIItem {
			UIType type = NONE;
			float x = 0;
			float y = 0;
			float width = 0;
			float height = 0;
			std::string text;
			uint32_t color = 0xFFFFFFFF;
			bool vectorScreenCoords = true;
			Id ownerScriptId = -1;
		};

		struct RunResult {
			bool succeeded;
			bool breakRequested;
		};

		using UIReqs = std::unordered_map<Id, UIItem>;
		using ScriptMap = std::unordered_map<Id, Script>;
		using LabelAddrFunc = std::function<int(const std::string&)>;

		explicit Scripts(LabelAddrFunc getLabelAddrFunc);
		~Scripts();

		auto Add(const nlohmann::json& input) -> const Script&;
		auto Edit(Id scriptId, const nlohmann::json& input) -> const Script&;
		auto Compile(Id scriptId) -> const Script&;
		auto RunOnce(Id scriptId, const CpuI8080::State* cpuState,
			const Memory::State* memState, const IO::State* ioState,
			const Display::State* displayState) -> RunResult;
		auto Disable(Id scriptId) -> const Script&;
		auto DisableAll() -> size_t;
		void Del(Id scriptId);
		void Clear();

		auto Check(const CpuI8080::State* cpuState, const Memory::State* memState,
			const IO::State* ioState, const Display::State* displayState) -> bool;
		auto Get(Id scriptId) const -> const Script&;
		auto GetAllJson() const -> nlohmann::json;
		auto GetUpdates() const -> uint32_t { return m_updates; }
		auto GetBreakScriptId() const -> Id { return m_breakScriptId; }
		auto GetUIItems() const -> UIReqs;
		void ClearUIItems();

	private:
		friend struct ScriptsTestAccess;

		void RegisterCppFunctions();
		void CompileScript(Script& script);
		auto RunScript(Script& script) -> RunResult;
		auto FindRequired(Id scriptId) -> Script&;
		void ReleaseReference(Script& script);
		void RemoveUIItems(Id scriptId);
		void SetExecutionState(const CpuI8080::State* cpuState,
			const Memory::State* memState, const IO::State* ioState,
			const Display::State* displayState);
		static void InstructionHook(lua_State* state, lua_Debug* debug);

		ScriptMap m_scripts;
		uint32_t m_updates = 0;
		Id m_nextScriptId = 0;
		bool m_idsExhausted = false;
		lua_State* m_luaState = nullptr;
		bool m_enabled = false;

		const CpuI8080::State* m_cpuStateP = nullptr;
		const Memory::State* m_memStateP = nullptr;
		const IO::State* m_ioStateP = nullptr;
		const Display::State* m_displayStateP = nullptr;
		bool m_break = false;
		Id m_currentScriptId = -1;
		Id m_breakScriptId = -1;
		int m_instructionCount = 0;
		std::chrono::steady_clock::time_point m_executionDeadline;

		UIReqs m_uiReqs;
		mutable std::mutex m_uiReqsMutex;
		LabelAddrFunc m_getLabelAddrFunc;
	};
}