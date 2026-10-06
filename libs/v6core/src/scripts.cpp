#include <algorithm>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <limits>
#include <string>
#include <type_traits>
#include <vector>

#include "core/scripts.h"
#include "utils/str_utils.h"
#include "utils/utils.h"

extern "C"
{
#include <luajit.h>
}

namespace
{
	constexpr const char* SCRIPTS_REGISTRY_KEY = "v6emul.scripts";

	auto IsValidUtf8(const std::string& value) -> bool
	{
		for (size_t index = 0; index < value.size();) {
			const auto first = static_cast<unsigned char>(value[index]);
			size_t length = 0;
			uint32_t codepoint = 0;
			if (first <= 0x7F) { length = 1; codepoint = first; }
			else if ((first & 0xE0) == 0xC0) { length = 2; codepoint = first & 0x1F; }
			else if ((first & 0xF0) == 0xE0) { length = 3; codepoint = first & 0x0F; }
			else if ((first & 0xF8) == 0xF0) { length = 4; codepoint = first & 0x07; }
			else return false;
			if (index + length > value.size()) return false;
			for (size_t offset = 1; offset < length; offset++) {
				const auto next = static_cast<unsigned char>(value[index + offset]);
				if ((next & 0xC0) != 0x80) return false;
				codepoint = (codepoint << 6) | (next & 0x3F);
			}
			if ((length == 2 && codepoint < 0x80) || (length == 3 && codepoint < 0x800) ||
				(length == 4 && codepoint < 0x10000) || codepoint > 0x10FFFF ||
				(codepoint >= 0xD800 && codepoint <= 0xDFFF)) return false;
			index += length;
		}
		return true;
	}

	auto BoundedError(std::string error) -> std::string
	{
		if (error.size() <= dev::Scripts::MAX_ERROR_BYTES) return error;
		error.resize(dev::Scripts::MAX_ERROR_BYTES);
		while (!error.empty() && !IsValidUtf8(error)) error.pop_back();
		return error;
	}

	auto LuaError(lua_State* state) -> std::string
	{
		const char* message = lua_tostring(state, -1);
		return BoundedError(message ? message : "Lua operation failed");
	}

	auto CheckStrictNumber(lua_State* state, int index, const char* functionName,
		const char* parameterName) -> double
	{
		if (lua_type(state, index) != LUA_TNUMBER)
			return luaL_error(state, "%s: %s must be a number", functionName, parameterName), 0.0;
		const double value = lua_tonumber(state, index);
		if (!std::isfinite(value))
			return luaL_error(state, "%s: %s must be finite", functionName, parameterName), 0.0;
		return value;
	}

	auto CheckStrictInteger(lua_State* state, int index, const char* functionName,
		const char* parameterName, double maximum) -> uint64_t
	{
		const double value = CheckStrictNumber(state, index, functionName, parameterName);
		if (value < 0 || value > maximum || std::trunc(value) != value)
			return luaL_error(state, "%s: %s must be an integer in range", functionName, parameterName), 0;
		return static_cast<uint64_t>(value);
	}
}

dev::Scripts::Scripts(LabelAddrFunc getLabelAddrFunc)
	: m_getLabelAddrFunc(std::move(getLabelAddrFunc))
{
	m_luaState = luaL_newstate();
	if (!m_luaState) {
		dev::Log("Scripts: Failed to create lua state. Script support is disabled.");
		return;
	}

	luaL_openlibs(m_luaState);
	luaJIT_setmode(m_luaState, 0, LUAJIT_MODE_ENGINE | LUAJIT_MODE_OFF);
	for (const char* global : {"io", "os", "package", "require", "loadfile", "dofile"}) {
		lua_pushnil(m_luaState);
		lua_setglobal(m_luaState, global);
	}
	lua_pushlightuserdata(m_luaState, this);
	lua_setfield(m_luaState, LUA_REGISTRYINDEX, SCRIPTS_REGISTRY_KEY);
	RegisterCppFunctions();

	m_enabled = true;
}

dev::Scripts::~Scripts()
{
	for (auto& [id, script] : m_scripts) ReleaseReference(script);
	if (m_luaState){
		lua_close(m_luaState);
	}
}

// Helper struct with template specializations for type handling
struct LuaGetter {
	template<typename T>
	static int pushValue(lua_State* state, T value) {
		return 0;
	}
};

// Specializations for supported types
template<> inline int LuaGetter::pushValue<bool>(
		lua_State* state, bool value)
{
	lua_pushboolean(state, value);
	return 1;
}
template<> inline int LuaGetter::pushValue<uint8_t>(
		lua_State* state, uint8_t value)
{
	lua_pushinteger(state, static_cast<lua_Integer>(value));
	return 1;
}
template<> inline int LuaGetter::pushValue<uint16_t>(
		lua_State* state, uint16_t value)
{
	lua_pushinteger(state, static_cast<lua_Integer>(value));
	return 1;
}
template<> inline int LuaGetter::pushValue<int>(
		lua_State* state, int value)
{
	lua_pushinteger(state, static_cast<lua_Integer>(value));
	return 1;
}
template<> inline int LuaGetter::pushValue<uint64_t>(
		lua_State* state, uint64_t value)
{
	lua_pushinteger(state, static_cast<lua_Integer>(value));
	return 1;
}
template<> inline int LuaGetter::pushValue<double>(
		lua_State* state, double value)
{
	lua_pushnumber(state, value);
	return 1;
}
template<> inline int LuaGetter::pushValue<const char*>(
	lua_State* state, const char* value)
{
	lua_pushstring(state, value);
	return 1;
}

#define REGISTER_STRUCT_FIELD_GETTER(LUA_STATE, STRUCT_PTR, FIELD_PATH, FUNC_NAME) \
	do { \
		lua_CFunction getterFunc = [](lua_State* state) -> int { \
			using StructType = std::remove_pointer_t<decltype(STRUCT_PTR)>; \
			auto* structPtr = static_cast<StructType**>(lua_touserdata(state, lua_upvalueindex(1))); \
			LuaGetter::pushValue(state, (*structPtr)->FIELD_PATH); \
			return 1; \
		}; \
		lua_pushlightuserdata(LUA_STATE, (void*)(&STRUCT_PTR)); \
		lua_pushcclosure(LUA_STATE, getterFunc, 1); \
		lua_setglobal(LUA_STATE, FUNC_NAME); \
	} while(0)

void dev::Scripts::RegisterCppFunctions()
{
	// Register a Break function
	lua_CFunction breakFunc = [](lua_State* _luaState) -> int {
		Scripts* scripts = static_cast<Scripts*>(lua_touserdata(_luaState, lua_upvalueindex(1)));
		scripts->m_break = true;
		return 0;
	};
	lua_pushcfunction(m_luaState, breakFunc);
	lua_pushlightuserdata(m_luaState, this);
	lua_pushcclosure(m_luaState, breakFunc, 1);
	lua_setglobal(m_luaState, "Break");

	// Register CPU state getters
	REGISTER_STRUCT_FIELD_GETTER(m_luaState, m_cpuStateP, cc, "GetCC");
	REGISTER_STRUCT_FIELD_GETTER(m_luaState, m_cpuStateP, regs.pc.word, "GetPC");
	REGISTER_STRUCT_FIELD_GETTER(m_luaState, m_cpuStateP, regs.sp.word, "GetSP");
	REGISTER_STRUCT_FIELD_GETTER(m_luaState, m_cpuStateP, regs.psw.af.word, "GetPSW");
	REGISTER_STRUCT_FIELD_GETTER(m_luaState, m_cpuStateP, regs.bc.word, "GetBC");
	REGISTER_STRUCT_FIELD_GETTER(m_luaState, m_cpuStateP, regs.de.word, "GetDE");
	REGISTER_STRUCT_FIELD_GETTER(m_luaState, m_cpuStateP, regs.hl.word, "GetHL");
	REGISTER_STRUCT_FIELD_GETTER(m_luaState, m_cpuStateP, regs.psw.a, "GetA");
	REGISTER_STRUCT_FIELD_GETTER(m_luaState, m_cpuStateP, regs.psw.af.l, "GetF");
	REGISTER_STRUCT_FIELD_GETTER(m_luaState, m_cpuStateP, regs.bc.h, "GetB");
	REGISTER_STRUCT_FIELD_GETTER(m_luaState, m_cpuStateP, regs.bc.l, "GetC");
	REGISTER_STRUCT_FIELD_GETTER(m_luaState, m_cpuStateP, regs.de.h, "GetD");
	REGISTER_STRUCT_FIELD_GETTER(m_luaState, m_cpuStateP, regs.de.l, "GetE");
	REGISTER_STRUCT_FIELD_GETTER(m_luaState, m_cpuStateP, regs.hl.h, "GetH");
	REGISTER_STRUCT_FIELD_GETTER(m_luaState, m_cpuStateP, regs.hl.l, "GetL");

	REGISTER_STRUCT_FIELD_GETTER(m_luaState, m_cpuStateP, regs.psw.s, "GetFlagS");
	REGISTER_STRUCT_FIELD_GETTER(m_luaState, m_cpuStateP, regs.psw.z, "GetFlagZ");
	REGISTER_STRUCT_FIELD_GETTER(m_luaState, m_cpuStateP, regs.psw.ac, "GetFlagAC");
	REGISTER_STRUCT_FIELD_GETTER(m_luaState, m_cpuStateP, regs.psw.p, "GetFlagP");
	REGISTER_STRUCT_FIELD_GETTER(m_luaState, m_cpuStateP, regs.psw.c, "GetFlagC");
	REGISTER_STRUCT_FIELD_GETTER(m_luaState, m_cpuStateP, ints.inte, "GetINTE");
	REGISTER_STRUCT_FIELD_GETTER(m_luaState, m_cpuStateP, ints.iff, "GetIFF");
	REGISTER_STRUCT_FIELD_GETTER(m_luaState, m_cpuStateP, ints.hlta, "GetHLTA");
	REGISTER_STRUCT_FIELD_GETTER(m_luaState, m_cpuStateP, ints.mc, "GetMachineCycles");

	// Register Memory state getters
	REGISTER_STRUCT_FIELD_GETTER(m_luaState, m_memStateP, debug.instr.opcode, "GetOpcode");

	lua_CFunction getterByteGlobal = [](lua_State* state) -> int {
		using StructType = std::remove_pointer_t<decltype(m_memStateP)>;
		auto* structPtr = static_cast<StructType**>(
			lua_touserdata(state, lua_upvalueindex(1)));

		int globalAddr = luaL_checkinteger(state, 1);
		int val = (*structPtr)->ramP->at(globalAddr);
		lua_pushinteger(state, val);
		return 1;
	};
	lua_pushlightuserdata(m_luaState, (void*)(&m_memStateP));
	lua_pushcclosure(m_luaState, getterByteGlobal, 1);
	lua_setglobal(m_luaState, "GetByteGlobal");

	// Get label addr
	lua_CFunction getterLabelAddr = [](lua_State* state) -> int {
		using StructType = std::remove_pointer_t<decltype(m_memStateP)>;

		auto* scriptsP = static_cast<Scripts*>(
			lua_touserdata(state, lua_upvalueindex(1)));

		const char* label = luaL_checkstring(state, 1);
		if (!label) {
			luaL_error(state, "GetLabelAddr: label argument must be a string");
			return 0;
		}

		if (scriptsP) {
			int addr = scriptsP->m_getLabelAddrFunc(label);
			lua_pushinteger(state, addr);
		}

		return 1;
	};
	lua_pushlightuserdata(m_luaState, (void*)(this));
	lua_pushcclosure(m_luaState, getterLabelAddr, 1);
	lua_setglobal(m_luaState, "GetLabelAddr");

	// DrawText
	lua_CFunction drawTextFunc = [](lua_State* state) -> int
	{
		const auto paramNum = lua_gettop(state);
		if (paramNum < 4 || paramNum > 6) {
			return luaL_error(state, "DrawText: expected (id, text, x, y, <color=0xFFFFFFFF>, <vectorScreenCoords=true>)");
		}
		const auto id = CheckStrictInteger(state, 1, "DrawText", "id", std::numeric_limits<int32_t>::max());
		if (lua_type(state, 2) != LUA_TSTRING) return luaL_error(state, "DrawText: text must be a string");
		size_t textLength = 0;
		const char* text = lua_tolstring(state, 2, &textLength);
		if (textLength > Scripts::MAX_OVERLAY_TEXT_BYTES) return luaL_error(state, "DrawText: text exceeds maxTextBytes");
		const std::string textValue(text, textLength);
		if (textValue.find('\0') != std::string::npos || !IsValidUtf8(textValue))
			return luaL_error(state, "DrawText: text must be valid UTF-8 without NUL bytes within maxTextBytes");
		const double xValue = CheckStrictNumber(state, 3, "DrawText", "x");
		const double yValue = CheckStrictNumber(state, 4, "DrawText", "y");
		if (std::abs(xValue) > Scripts::MAX_OVERLAY_COORDINATE_MAGNITUDE || std::abs(yValue) > Scripts::MAX_OVERLAY_COORDINATE_MAGNITUDE)
			return luaL_error(state, "DrawText: coordinates are out of range");
		const float x = static_cast<float>(xValue);
		const float y = static_cast<float>(yValue);
		uint32_t color = 0xFFFFFFFF;
		if (paramNum >= 5) {
			color = static_cast<uint32_t>(CheckStrictInteger(state, 5, "DrawText", "color",
				std::numeric_limits<uint32_t>::max()));
		}
		bool vectorScreenCoords = true;
		if (paramNum >= 6) {
			if (!lua_isboolean(state, 6)) return luaL_error(state, "DrawText: vectorScreenCoords must be boolean");
			vectorScreenCoords = lua_toboolean(state, 6) != 0;
		}
		auto* scripts = static_cast<Scripts*>(lua_touserdata(state, lua_upvalueindex(1)));
		if (scripts) scripts->StoreOverlay(static_cast<Id>(id), {Scripts::OverlayType::TEXT, x, y, 0, 0, textValue, color, vectorScreenCoords, false, true});
		return 0;
	};
	lua_pushlightuserdata(m_luaState, (void*)(this));
	lua_pushcclosure(m_luaState, drawTextFunc, 1);
	lua_setglobal(m_luaState, "DrawText");

	// DrawRect
	lua_CFunction drawRectFunc = [](lua_State* state) -> int
	{
		const auto paramNum = lua_gettop(state);
		if (paramNum < 5 || paramNum > 8) {
			return luaL_error(state, "DrawRect: expected (id, x, y, width, height, <filled=false>, <color=0xFFFFFFFF>, <vectorScreenCoords=true>)");
		}
		const auto id = CheckStrictInteger(state, 1, "DrawRect", "id", std::numeric_limits<int32_t>::max());
		const double xValue = CheckStrictNumber(state, 2, "DrawRect", "x");
		const double yValue = CheckStrictNumber(state, 3, "DrawRect", "y");
		const double widthValue = CheckStrictNumber(state, 4, "DrawRect", "width");
		const double heightValue = CheckStrictNumber(state, 5, "DrawRect", "height");
		if (std::abs(xValue) > Scripts::MAX_OVERLAY_COORDINATE_MAGNITUDE || std::abs(yValue) > Scripts::MAX_OVERLAY_COORDINATE_MAGNITUDE ||
			widthValue < 0 || heightValue < 0 || widthValue > Scripts::MAX_OVERLAY_COORDINATE_MAGNITUDE || heightValue > Scripts::MAX_OVERLAY_COORDINATE_MAGNITUDE)
			return luaL_error(state, "DrawRect: coordinates or dimensions are out of range");
		const float x = static_cast<float>(xValue);
		const float y = static_cast<float>(yValue);
		const float width = static_cast<float>(widthValue);
		const float height = static_cast<float>(heightValue);
		bool filled = false;
		if (paramNum >= 6) {
			if (!lua_isboolean(state, 6)) return luaL_error(state, "DrawRect: filled must be boolean");
			filled = lua_toboolean(state, 6) != 0;
		}
		uint32_t color = 0xFFFFFFFF;
		bool vectorScreenCoords = true;
		if (paramNum >= 7) {
			color = static_cast<uint32_t>(CheckStrictInteger(state, 7, "DrawRect", "color",
				std::numeric_limits<uint32_t>::max()));
		}
		if (paramNum >= 8) {
			if (!lua_isboolean(state, 8)) return luaL_error(state, "DrawRect: vectorScreenCoords must be boolean");
			vectorScreenCoords = lua_toboolean(state, 8) != 0;
		}
		auto* scripts = static_cast<Scripts*>(lua_touserdata(state, lua_upvalueindex(1)));
		if (scripts) scripts->StoreOverlay(static_cast<Id>(id), {Scripts::OverlayType::RECT, x, y, width, height, "", color, vectorScreenCoords, filled, true});
		return 0;
	};
	lua_pushlightuserdata(m_luaState, (void*)(this));
	lua_pushcclosure(m_luaState, drawRectFunc, 1);
	lua_setglobal(m_luaState, "DrawRect");

}

auto dev::Scripts::FindRequired(Id scriptId) -> Script&
{
	auto script = m_scripts.find(scriptId);
	if (script == m_scripts.end()) throw ScriptNotFound(scriptId);
	return script->second;
}

void dev::Scripts::ReleaseReference(Script& script)
{
	if (script.ref != LUA_NOREF && script.ref != LUA_REFNIL) {
		luaL_unref(m_luaState, LUA_REGISTRYINDEX, script.ref);
	}
	script.ref = LUA_NOREF;
}

void dev::Scripts::CompileScript(Script& script)
{
	ReleaseReference(script);
	script.runtimeStatus = ScriptRuntimeStatus::NEVER_RUN;
	script.runtimeError.clear();

	std::error_code errorCode;
	const auto nativePath = std::filesystem::path(
		reinterpret_cast<const char8_t*>(script.path.c_str()));
	if (!std::filesystem::is_regular_file(nativePath, errorCode)) {
		script.compilationStatus = ScriptCompilationStatus::ERROR;
		script.compilationError = "Script source is missing, unreadable, or not a regular file";
		return;
	}
	const auto fileSize = std::filesystem::file_size(nativePath, errorCode);
	if (errorCode || fileSize > MAX_SOURCE_BYTES) {
		script.compilationStatus = ScriptCompilationStatus::ERROR;
		script.compilationError = errorCode ? "Script source size cannot be read" :
			"Script source exceeds maxSourceBytes";
		return;
	}

	std::ifstream file(nativePath, std::ios::binary);
	if (!file) {
		script.compilationStatus = ScriptCompilationStatus::ERROR;
		script.compilationError = "Script source cannot be read";
		return;
	}
	std::string source(static_cast<size_t>(fileSize), '\0');
	if (!source.empty()) file.read(source.data(), static_cast<std::streamsize>(source.size()));
	if (!file || source.find('\0') != std::string::npos || !IsValidUtf8(source)) {
		script.compilationStatus = ScriptCompilationStatus::ERROR;
		script.compilationError = "Script source must be valid UTF-8 without NUL bytes";
		return;
	}

	if (luaL_loadbuffer(m_luaState, source.data(), source.size(), script.path.c_str()) != LUA_OK) {
		script.compilationStatus = ScriptCompilationStatus::ERROR;
		script.compilationError = LuaError(m_luaState);
		lua_pop(m_luaState, 1);
		return;
	}
	script.ref = luaL_ref(m_luaState, LUA_REGISTRYINDEX);
	script.compilationStatus = ScriptCompilationStatus::COMPILED;
	script.compilationError.clear();
}

auto dev::Scripts::Add(const nlohmann::json& input) -> const Script&
{
	if (!m_enabled) throw std::runtime_error("Lua scripting is unavailable");
	if (m_scripts.size() >= MAX_RECORDS) throw ScriptAddError(ScriptAddFailure::CAPACITY);
	if (m_idsExhausted) throw ScriptAddError(ScriptAddFailure::ID_EXHAUSTED);

	const Id scriptId = m_nextScriptId;
	if (m_nextScriptId == std::numeric_limits<Id>::max()) m_idsExhausted = true;
	else m_nextScriptId++;
	auto [position, inserted] = m_scripts.emplace(scriptId, Script{
		scriptId, input.at("name").get<std::string>(), input.at("path").get<std::string>(),
		input.at("active").get<bool>()});
	CompileScript(position->second);
	m_updates++;
	return position->second;
}

auto dev::Scripts::Edit(Id scriptId, const nlohmann::json& input) -> const Script&
{
	auto& script = FindRequired(scriptId);
	const auto name = input.at("name").get<std::string>();
	const auto path = input.at("path").get<std::string>();
	const auto active = input.at("active").get<bool>();
	if (script.name == name && script.path == path && script.active == active) {
		if (!active) RemoveOverlays(scriptId);
		return script;
	}

	const bool pathChanged = script.path != path;
	script.name = name;
	script.path = path;
	script.active = active;
	if (pathChanged) CompileScript(script);
	if (!active) RemoveOverlays(scriptId);
	m_updates++;
	return script;
}

auto dev::Scripts::Compile(Id scriptId) -> const Script&
{
	auto& script = FindRequired(scriptId);
	CompileScript(script);
	m_updates++;
	return script;
}

void dev::Scripts::SetExecutionState(const CpuI8080::State* cpuState,
	const Memory::State* memState, const IO::State* ioState,
	const Display::State* displayState)
{
	m_cpuStateP = cpuState;
	m_memStateP = memState;
	m_ioStateP = ioState;
	m_displayStateP = displayState;
}

void dev::Scripts::InstructionHook(lua_State* state, lua_Debug*)
{
	lua_getfield(state, LUA_REGISTRYINDEX, SCRIPTS_REGISTRY_KEY);
	auto* scripts = static_cast<Scripts*>(lua_touserdata(state, -1));
	lua_pop(state, 1);
	if (!scripts) luaL_error(state, "Script execution context is unavailable");
	scripts->m_instructionCount += 1000;
	if (scripts->m_instructionCount > MAX_INSTRUCTIONS_PER_RUN ||
		std::chrono::steady_clock::now() > scripts->m_executionDeadline) {
		luaL_error(state, "Script execution budget exceeded");
	}
}

auto dev::Scripts::RunScript(Script& script) -> RunResult
{
	if (script.compilationStatus != ScriptCompilationStatus::COMPILED ||
		script.ref == LUA_NOREF) throw ScriptNotCompiled(script.scriptId);

	const auto previousStatus = script.runtimeStatus;
	const auto previousError = script.runtimeError;
	m_break = false;
	m_currentScriptId = script.scriptId;
	m_instructionCount = 0;
	m_executionDeadline = std::chrono::steady_clock::now() +
		std::chrono::milliseconds(MAX_EXECUTION_MILLISECONDS);
	lua_sethook(m_luaState, InstructionHook, LUA_MASKCOUNT, 1000);
	lua_rawgeti(m_luaState, LUA_REGISTRYINDEX, script.ref);
	const int status = lua_pcall(m_luaState, 0, 0, 0);
	lua_sethook(m_luaState, nullptr, 0, 0);
	m_currentScriptId = -1;

	if (status == LUA_OK) {
		script.runtimeStatus = ScriptRuntimeStatus::SUCCEEDED;
		script.runtimeError.clear();
	} else {
		script.runtimeStatus = ScriptRuntimeStatus::ERROR;
		script.runtimeError = LuaError(m_luaState);
		lua_pop(m_luaState, 1);
	}
	if (m_break) m_breakScriptId = script.scriptId;
	if (previousStatus != script.runtimeStatus || previousError != script.runtimeError) m_updates++;
	return {status == LUA_OK, m_break};
}

auto dev::Scripts::RunOnce(Id scriptId, const CpuI8080::State* cpuState,
	const Memory::State* memState, const IO::State* ioState,
	const Display::State* displayState) -> RunResult
{
	SetExecutionState(cpuState, memState, ioState, displayState);
	return RunScript(FindRequired(scriptId));
}

auto dev::Scripts::Disable(Id scriptId) -> const Script&
{
	auto& script = FindRequired(scriptId);
	RemoveOverlays(scriptId);
	if (script.active) {
		script.active = false;
		m_updates++;
	}
	return script;
}

auto dev::Scripts::DisableAll() -> size_t
{
	size_t disabled = 0;
	for (auto& [id, script] : m_scripts) {
		RemoveOverlays(id);
		if (!script.active) continue;
		script.active = false;
		disabled++;
	}
	if (disabled > 0) m_updates++;
	return disabled;
}

void dev::Scripts::Del(Id scriptId)
{
	auto script = m_scripts.find(scriptId);
	if (script == m_scripts.end()) return;
	ReleaseReference(script->second);
	RemoveOverlays(scriptId);
	m_scripts.erase(script);
	m_updates++;
}

void dev::Scripts::Clear()
{
	if (m_scripts.empty()) return;
	for (auto& [id, script] : m_scripts) ReleaseReference(script);
	m_scripts.clear();
	ClearOverlays();
	m_updates++;
}

auto dev::Scripts::Check(const CpuI8080::State* cpuState, const Memory::State* memState,
	const IO::State* ioState, const Display::State* displayState) -> bool
{
	if (!m_enabled) return false;
	SetExecutionState(cpuState, memState, ioState, displayState);
	bool breakRequested = false;
	for (auto& [id, script] : m_scripts) {
		if (!script.active || script.compilationStatus != ScriptCompilationStatus::COMPILED ||
			script.runtimeStatus == ScriptRuntimeStatus::ERROR) continue;
		breakRequested |= RunScript(script).breakRequested;
	}
	return breakRequested;
}

auto dev::Scripts::GetAllJson() const -> nlohmann::json
{
	std::vector<const Script*> ordered;
	ordered.reserve(m_scripts.size());
	for (const auto& [id, script] : m_scripts) ordered.push_back(&script);
	std::sort(ordered.begin(), ordered.end(), [](const Script* lhs, const Script* rhs) {
		return lhs->scriptId < rhs->scriptId;
	});
	nlohmann::json scripts = nlohmann::json::array();
	for (const auto* script : ordered) scripts.push_back(script->ToJson());
	return {{"updates", m_updates}, {"scripts", std::move(scripts)}};
}

auto dev::Scripts::Get(Id scriptId) const -> const Script&
{
	auto script = m_scripts.find(scriptId);
	if (script == m_scripts.end()) throw ScriptNotFound(scriptId);
	return script->second;
}

void dev::Scripts::StoreOverlay(Id itemId, OverlayItem item)
{
	if (m_currentScriptId < 0) return;
	auto& scriptOverlays = m_overlays[m_currentScriptId];
	auto existing = scriptOverlays.find(itemId);
	if (existing == scriptOverlays.end()) {
		if (scriptOverlays.size() >= MAX_OVERLAYS_PER_SCRIPT) luaL_error(m_luaState, "Overlay limit per script exceeded");
		size_t total = 0;
		for (const auto& [scriptId, overlays] : m_overlays) total += overlays.size();
		if (total >= MAX_OVERLAYS_TOTAL) luaL_error(m_luaState, "Overlay total limit exceeded");
		scriptOverlays.emplace(itemId, std::move(item));
		return;
	}
	const bool changed = existing->second.type != item.type || existing->second.x != item.x || existing->second.y != item.y ||
		existing->second.width != item.width || existing->second.height != item.height || existing->second.text != item.text ||
		existing->second.color != item.color || existing->second.vectorScreenCoords != item.vectorScreenCoords || existing->second.filled != item.filled;
	if (!changed) return;
	item.updated = true;
	existing->second = std::move(item);
}

auto dev::Scripts::GetOverlayUpdatesJson() -> nlohmann::json
{
	nlohmann::json overlays = nlohmann::json::array();
	std::vector<Id> scriptIds;
	for (const auto& [scriptId, items] : m_overlays) scriptIds.push_back(scriptId);
	std::sort(scriptIds.begin(), scriptIds.end());
	for (const auto scriptId : scriptIds) {
		auto& items = m_overlays.at(scriptId);
		std::vector<Id> itemIds;
		for (const auto& [itemId, item] : items) if (item.updated) itemIds.push_back(itemId);
		std::sort(itemIds.begin(), itemIds.end());
		for (const auto itemId : itemIds) {
			auto& item = items.at(itemId);
			nlohmann::json overlay = {{"scriptId", scriptId}, {"itemId", itemId}, {"vectorScreenCoords", item.vectorScreenCoords}, {"x", item.x}, {"y", item.y}, {"color", item.color}};
			if (item.type == OverlayType::TEXT) overlay["type"] = "text", overlay["text"] = item.text;
			else overlay["type"] = "rect", overlay["width"] = item.width, overlay["height"] = item.height, overlay["filled"] = item.filled;
			item.updated = false;
			overlays.push_back(std::move(overlay));
		}
	}
	return {{"overlays", std::move(overlays)}};
}

void dev::Scripts::MarkAllOverlaysUpdated()
{
	for (auto& [scriptId, items] : m_overlays)
		for (auto& [itemId, item] : items) item.updated = true;
}

void dev::Scripts::RemoveOverlays(Id scriptId)
{
	m_overlays.erase(scriptId);
}

void dev::Scripts::ClearOverlays()
{
	m_overlays.clear();
}