#pragma once

#include <stdexcept>
#include <string>

#include <lua.hpp>
#include <nlohmann/json.hpp>

#include "utils/types.h"

namespace dev
{
	enum class ScriptCompilationStatus { COMPILED, ERROR };
	enum class ScriptRuntimeStatus { NEVER_RUN, SUCCEEDED, ERROR };

	struct Script
	{
		Script(Id _scriptId, std::string _name, std::string _path, bool _active);

		auto ToJson() const -> nlohmann::json;

		Id scriptId;
		std::string name;
		std::string path;
		bool active;
		ScriptCompilationStatus compilationStatus = ScriptCompilationStatus::ERROR;
		std::string compilationError;
		ScriptRuntimeStatus runtimeStatus = ScriptRuntimeStatus::NEVER_RUN;
		std::string runtimeError;
		int ref = LUA_NOREF;
	};

	class ScriptNotFound : public std::runtime_error
	{
	public:
		explicit ScriptNotFound(Id scriptId)
			: std::runtime_error("script id not found"), m_scriptId(scriptId) {}

		auto GetScriptId() const -> Id { return m_scriptId; }

	private:
		Id m_scriptId;
	};

	enum class ScriptAddFailure { CAPACITY, ID_EXHAUSTED };

	class ScriptAddError : public std::runtime_error
	{
	public:
		explicit ScriptAddError(ScriptAddFailure failure)
			: std::runtime_error("script cannot be added"), m_failure(failure) {}

		auto GetFailure() const -> ScriptAddFailure { return m_failure; }

	private:
		ScriptAddFailure m_failure;
	};

	class ScriptNotCompiled : public std::runtime_error
	{
	public:
		explicit ScriptNotCompiled(Id scriptId)
			: std::runtime_error("script is not compiled"), m_scriptId(scriptId) {}

		auto GetScriptId() const -> Id { return m_scriptId; }

	private:
		Id m_scriptId;
	};
}
