#include <string>
#include <iomanip>
#include <sstream>
#include <cstring>
#include <vector>

#include "core/script.h"
#include "utils/str_utils.h"
#include "utils/utils.h"

auto CompilationToJson(const dev::Script& script) -> nlohmann::json
{
	if (script.compilationStatus == dev::ScriptCompilationStatus::COMPILED) {
		return {{"status", "compiled"}, {"error", nullptr}};
	}
	return {{"status", "error"}, {"error", script.compilationError}};
}

auto RuntimeToJson(const dev::Script& script) -> nlohmann::json
{
	switch (script.runtimeStatus) {
	case dev::ScriptRuntimeStatus::NEVER_RUN:
		return {{"status", "never_run"}, {"error", nullptr}};
	case dev::ScriptRuntimeStatus::SUCCEEDED:
		return {{"status", "succeeded"}, {"error", nullptr}};
	case dev::ScriptRuntimeStatus::ERROR:
		return {{"status", "error"}, {"error", script.runtimeError}};
	}
	return nullptr;
}

dev::Script::Script(Id _scriptId, std::string _name, std::string _path, bool _active)
	: scriptId(_scriptId), name(std::move(_name)), path(std::move(_path)), active(_active)
{}

auto dev::Script::ToJson() const -> nlohmann::json
{
	return {
		{"scriptId", scriptId},
		{"name", name},
		{"path", path},
		{"active", active},
		{"compilation", CompilationToJson(*this)},
		{"runtime", RuntimeToJson(*this)}
	};
}
