#include "ipc_request.h"

#include <cstdint>
#include <limits>

#include "core/hardware.h"
#include "ipc/commands.h"

namespace
{
	auto ReadCommand(const nlohmann::json& value, int& command) -> bool
	{
		if (value.is_number_unsigned()) {
			auto unsignedValue = value.get<uint64_t>();
			if (unsignedValue > static_cast<uint64_t>(std::numeric_limits<int>::max())) return false;
			command = static_cast<int>(unsignedValue);
			return true;
		}

		if (!value.is_number_integer()) return false;
		auto signedValue = value.get<int64_t>();
		if (signedValue < std::numeric_limits<int>::min() || signedValue > std::numeric_limits<int>::max()) return false;
		command = static_cast<int>(signedValue);
		return true;
	}

	auto IsSupportedCommand(const int command) -> bool
	{
		if (command == dev::ipc::CMD_PING ||
			command == dev::ipc::CMD_GET_FRAME ||
			command == dev::ipc::CMD_GET_FRAME_RAW ||
			command == dev::ipc::CMD_GET_SERVER_INFO) {
			return true;
		}

		return command >= static_cast<int>(dev::Hardware::Req::RUN) &&
			command <= static_cast<int>(dev::Hardware::Req::MOUNT_FDD);
	}

	auto IsAddress(const nlohmann::json& value) -> bool
	{
		if (value.is_number_unsigned()) return value.get<uint64_t>() <= 0xFFFF;
		if (!value.is_number_integer()) return false;
		auto address = value.get<int64_t>();
		return address >= 0 && address <= 0xFFFF;
	}
}

auto dev::server::ValidateRequest(const nlohmann::json& request) -> RequestValidation
{
	if (!request.is_object()) {
		return RequestError{"invalid_request", "request must be an object"};
	}

	auto commandIt = request.find(dev::ipc::FIELD_CMD);
	int command = 0;
	if (commandIt == request.end() || !ReadCommand(*commandIt, command)) {
		return RequestError{"invalid_request", "cmd must be a 32-bit integer"};
	}

	if (!IsSupportedCommand(command)) {
		return RequestError{"unknown_command", "unsupported command"};
	}

	nlohmann::json data = nlohmann::json::object();
	auto dataIt = request.find(dev::ipc::FIELD_DATA);
	if (dataIt != request.end()) {
		if (!dataIt->is_object()) {
			return RequestError{"invalid_request", "data must be an object"};
		}
		data = *dataIt;
	}

	if (command == static_cast<int>(dev::Hardware::Req::GET_STACK_SAMPLE)) {
		auto addressIt = data.find("addr");
		if (addressIt == data.end() || !IsAddress(*addressIt)) {
			return RequestError{"invalid_request", "GET_STACK_SAMPLE requires addr in the range 0..65535"};
		}
	}

	return IpcRequest{command, std::move(data)};
}

auto dev::server::MakeServerInfo(const std::string& emulatorVersion) -> nlohmann::json
{
	nlohmann::json commands = {
		dev::ipc::CMD_GET_SERVER_INFO,
		dev::ipc::CMD_GET_FRAME_RAW,
		dev::ipc::CMD_GET_FRAME,
		dev::ipc::CMD_PING
	};
	for (int command = static_cast<int>(dev::Hardware::Req::RUN);
		command <= static_cast<int>(dev::Hardware::Req::MOUNT_FDD); ++command) {
		commands.push_back(command);
	}

	return {
		{"protocolVersion", dev::ipc::PROTOCOL_VERSION},
		{"emulatorVersion", emulatorVersion},
		{"commands", std::move(commands)},
		{"capabilities", {
			{"debugger", true},
			{"rawFrame", true},
			{"rawFrameSchema", 1},
			{"stackSampleSchema", 1}
		}}
	};
}