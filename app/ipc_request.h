#pragma once

#include <string>
#include <variant>

#include <nlohmann/json.hpp>

namespace dev::server
{
	struct IpcRequest {
		int command;
		nlohmann::json data;
	};

	struct RequestError {
		std::string code;
		std::string message;
		nlohmann::json details = nlohmann::json::object();
	};

	using RequestValidation = std::variant<IpcRequest, RequestError>;

	auto ValidateRequest(const nlohmann::json& request) -> RequestValidation;
	auto MakeServerInfo(const std::string& emulatorVersion) -> nlohmann::json;
}