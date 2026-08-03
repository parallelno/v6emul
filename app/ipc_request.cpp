#include "ipc_request.h"

#include <cstdint>
#include <limits>
#include <string_view>
#include <unordered_set>

#include "core/breakpoint.h"
#include "core/code_perf.h"
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
			command <= static_cast<int>(dev::Hardware::Req::DEBUG_CODE_PERF_EDIT);
	}

	auto IsAddress(const nlohmann::json& value) -> bool
	{
		if (value.is_number_unsigned()) return value.get<uint64_t>() <= 0xFFFF;
		if (!value.is_number_integer()) return false;
		auto address = value.get<int64_t>();
		return address >= 0 && address <= 0xFFFF;
	}

	auto ReadUnsigned(const nlohmann::json& value, uint64_t& result) -> bool
	{
		if (value.is_number_unsigned()) {
			result = value.get<uint64_t>();
			return true;
		}
		if (!value.is_number_integer()) return false;
		const auto signedValue = value.get<int64_t>();
		if (signedValue < 0) return false;
		result = static_cast<uint64_t>(signedValue);
		return true;
	}

	auto IsValidUtf8(const std::string& value) -> bool
	{
		for (size_t index = 0; index < value.size();) {
			const auto first = static_cast<uint8_t>(value[index]);
			if (first <= 0x7F) {
				index++;
				continue;
			}

			size_t continuationCount = 0;
			uint32_t codePoint = 0;
			if (first >= 0xC2 && first <= 0xDF) {
				continuationCount = 1;
				codePoint = first & 0x1F;
			} else if (first >= 0xE0 && first <= 0xEF) {
				continuationCount = 2;
				codePoint = first & 0x0F;
			} else if (first >= 0xF0 && first <= 0xF4) {
				continuationCount = 3;
				codePoint = first & 0x07;
			} else {
				return false;
			}

			if (index + continuationCount >= value.size()) return false;
			for (size_t offset = 1; offset <= continuationCount; offset++) {
				const auto continuation = static_cast<uint8_t>(value[index + offset]);
				if ((continuation & 0xC0) != 0x80) return false;
				codePoint = (codePoint << 6) | (continuation & 0x3F);
			}
			if ((continuationCount == 2 && codePoint < 0x800) ||
				(continuationCount == 3 && codePoint < 0x10000) ||
				(codePoint >= 0xD800 && codePoint <= 0xDFFF) || codePoint > 0x10FFFF) return false;
			index += continuationCount + 1;
		}
		return true;
	}

	auto ValidateStructuredWatchpoint(const nlohmann::json& data, const int command, const bool requireId = false)
		-> std::optional<dev::server::RequestError>
	{
		constexpr size_t MAX_COMMENT_BYTES = 1024;
		const std::unordered_set<std::string_view> fields = {
			"globalAddr", "len", "value", "access",
			"condition", "type", "active", "comment"
		};
		for (const auto& [name, value] : data.items()) {
			if (!fields.contains(name) && !(requireId && name == "id")) {
				return dev::server::RequestError{"invalid_request",
					"DEBUG_WATCHPOINT_ADD contains unknown field: " + name,
					{{"command", command}, {"field", name}}};
			}
		}

		auto invalid = [command](const std::string& field, const std::string& requirement) {
			return dev::server::RequestError{"invalid_request",
				"command " + std::to_string(command) + " field " + field + " " + requirement,
				{{"command", command}, {"field", field}}};
		};

		uint64_t address = 0;
		uint64_t length = 0;
		uint64_t value = 0;
		uint64_t id = 0;
		if (requireId && (!data.contains("id") || !ReadUnsigned(data["id"], id) ||
			id > static_cast<uint64_t>(std::numeric_limits<dev::Id>::max())))
			return invalid("id", "must be a non-negative integer");
		if (!data.contains("globalAddr") || !ReadUnsigned(data["globalAddr"], address))
			return invalid("globalAddr", "must be an unsigned integer");
		if (!data.contains("len") || !ReadUnsigned(data["len"], length) || length == 0)
			return invalid("len", "must be a positive integer");
		if (address >= dev::Memory::MEMORY_GLOBAL_LEN || length > dev::Memory::MEMORY_GLOBAL_LEN - address)
			return invalid("len", "must define a range inside global memory");
		if (!data.contains("value") || !ReadUnsigned(data["value"], value))
			return invalid("value", "must be an unsigned integer");

		if (!data.contains("access") || !data["access"].is_string() ||
			(data["access"] != "R" && data["access"] != "W" && data["access"] != "RW"))
			return invalid("access", "must be R, W, or RW");
		if (!data.contains("condition") || !data["condition"].is_string() ||
			!std::unordered_set<std::string>{"ANY", "EQU", "LESS", "GREATER", "LESS_EQU", "GREATER_EQU", "NOT_EQU"}.contains(data["condition"]))
			return invalid("condition", "has an unsupported value");
		if (!data.contains("type") || !data["type"].is_string() ||
			(data["type"] != "LEN" && data["type"] != "WORD"))
			return invalid("type", "must be LEN or WORD");
		if (data["type"] == "WORD" && length != 2)
			return invalid("len", "must be 2 for WORD watchpoints");
		if (value > (data["type"] == "WORD" ? 0xFFFFu : 0xFFu))
			return invalid("value", "is outside the range for the selected type");
		if (!data.contains("active") || !data["active"].is_boolean())
			return invalid("active", "must be boolean");
		if (!data.contains("comment") || !data["comment"].is_string() ||
			data["comment"].get_ref<const std::string&>().size() > MAX_COMMENT_BYTES ||
			!IsValidUtf8(data["comment"].get_ref<const std::string&>()))
			return invalid("comment", "must be a UTF-8 string of at most 1024 bytes");

		return std::nullopt;
	}

	auto ValidateMemoryEditInput(const nlohmann::json& data, const int command)
		-> std::optional<dev::server::RequestError>
	{
		constexpr size_t MAX_COMMENT_BYTES = 1024;
		const std::unordered_set<std::string_view> fields = {
			"globalAddr", "enteredValue", "readonly", "active", "comment"
		};
		auto invalid = [command](const std::string& field, const std::string& requirement) {
			return dev::server::RequestError{"invalid_request",
				"command " + std::to_string(command) + " field " + field + " " + requirement,
				{{"command", command}, {"field", field}}};
		};
		for (const auto& [name, fieldValue] : data.items()) {
			if (!fields.contains(name)) return invalid(name, "is not supported");
		}

		uint64_t globalAddr = 0;
		uint64_t enteredValue = 0;
		if (!data.contains("globalAddr") || !ReadUnsigned(data["globalAddr"], globalAddr) ||
			globalAddr >= dev::Memory::MEMORY_GLOBAL_LEN)
			return invalid("globalAddr", "must be an integer inside global memory");
		if (!data.contains("enteredValue") || !ReadUnsigned(data["enteredValue"], enteredValue) ||
			enteredValue > 0xFF)
			return invalid("enteredValue", "must be an integer in the range 0..255");
		if (!data.contains("readonly") || !data["readonly"].is_boolean())
			return invalid("readonly", "must be boolean");
		if (!data.contains("active") || !data["active"].is_boolean())
			return invalid("active", "must be boolean");
		if (!data.contains("comment") || !data["comment"].is_string() ||
			data["comment"].get_ref<const std::string&>().size() > MAX_COMMENT_BYTES ||
			!IsValidUtf8(data["comment"].get_ref<const std::string&>()))
			return invalid("comment", "must be a UTF-8 string of at most 1024 bytes");
		return std::nullopt;
	}

	auto ValidateMemoryEditAddress(const nlohmann::json& data, const int command)
		-> std::optional<dev::server::RequestError>
	{
		auto invalid = [command](const std::string& field, const std::string& requirement) {
			return dev::server::RequestError{"invalid_request",
				"command " + std::to_string(command) + " field " + field + " " + requirement,
				{{"command", command}, {"field", field}}};
		};
		for (const auto& [name, fieldValue] : data.items()) {
			if (name != "globalAddr") return invalid(name, "is not supported");
		}
		uint64_t globalAddr = 0;
		if (!data.contains("globalAddr") || !ReadUnsigned(data["globalAddr"], globalAddr) ||
			globalAddr >= dev::Memory::MEMORY_GLOBAL_LEN)
			return invalid("globalAddr", "must be an integer inside global memory");
		return std::nullopt;
	}

	auto ValidateCodePerfInput(const nlohmann::json& data, const int command,
		const bool requireId = false) -> std::optional<dev::server::RequestError>
	{
		const std::unordered_set<std::string_view> fields = {
			"name", "addrStart", "addrEnd", "active"
		};
		auto invalid = [command](const std::string& field, const std::string& requirement) {
			return dev::server::RequestError{"invalid_request",
				"command " + std::to_string(command) + " field " + field + " " + requirement,
				{{"command", command}, {"field", field}}};
		};
		for (const auto& [name, fieldValue] : data.items()) {
			if (!fields.contains(name) && !(requireId && name == "id"))
				return invalid(name, "is not supported");
		}

		uint64_t id = 0;
		if (requireId && (!data.contains("id") || !ReadUnsigned(data["id"], id) ||
			id > static_cast<uint64_t>(std::numeric_limits<dev::Id>::max())))
			return invalid("id", "must be a non-negative integer representable by the server");
		if (!data.contains("name") || !data["name"].is_string() ||
			data["name"].get_ref<const std::string&>().size() > dev::CodePerf::MAX_NAME_BYTES ||
			!IsValidUtf8(data["name"].get_ref<const std::string&>()))
			return invalid("name", "must be a UTF-8 string of at most 1024 bytes");
		if (!data.contains("addrStart") || !IsAddress(data["addrStart"]))
			return invalid("addrStart", "must be an integer in the range 0..65535");
		if (!data.contains("addrEnd") || !IsAddress(data["addrEnd"]))
			return invalid("addrEnd", "must be an integer in the range 0..65535");
		if (data["addrStart"].get<uint64_t>() >= data["addrEnd"].get<uint64_t>())
			return invalid("addrEnd", "must be greater than addrStart");
		if (!data.contains("active") || !data["active"].is_boolean())
			return invalid("active", "must be boolean");
		return std::nullopt;
	}

	auto ValidateCodePerfId(const nlohmann::json& data, const int command)
		-> std::optional<dev::server::RequestError>
	{
		auto invalid = [command](const std::string& field, const std::string& requirement) {
			return dev::server::RequestError{"invalid_request",
				"command " + std::to_string(command) + " field " + field + " " + requirement,
				{{"command", command}, {"field", field}}};
		};
		for (const auto& [name, fieldValue] : data.items()) {
			if (name != "id") return invalid(name, "is not supported");
		}
		uint64_t id = 0;
		if (!data.contains("id") || !ReadUnsigned(data["id"], id) ||
			id > static_cast<uint64_t>(std::numeric_limits<dev::Id>::max()))
			return invalid("id", "must be a non-negative integer representable by the server");
		return std::nullopt;
	}

	auto ValidateBreakpointAddress(const nlohmann::json& data, const int command,
		const bool allowStatus = false) -> std::optional<dev::server::RequestError>
	{
		const auto expectedSize = allowStatus ? 2u : 1u;
		if (data.size() != expectedSize || !data.contains("addr") || !IsAddress(data["addr"])) {
			return dev::server::RequestError{"invalid_request",
				"command " + std::to_string(command) + " field addr must be an integer in the range 0..65535",
				{{"command", command}, {"field", "addr"}}};
		}
		if (allowStatus && (!data.contains("status") || !data["status"].is_string() ||
			(data["status"] != "ACTIVE" && data["status"] != "DISABLED"))) {
			return dev::server::RequestError{"invalid_request",
				"command " + std::to_string(command) + " field status must be ACTIVE or DISABLED",
				{{"command", command}, {"field", "status"}}};
		}
		return std::nullopt;
	}

	auto ValidateStructuredBreakpoint(const nlohmann::json& data, const int command)
		-> std::optional<dev::server::RequestError>
	{
		constexpr size_t MAX_COMMENT_BYTES = 1024;
		const std::unordered_set<std::string_view> fields = {
			"addr", "memPages", "status", "autoDelete", "operand",
			"condition", "value", "comment"
		};
		auto invalid = [command](const std::string& field, const std::string& requirement) {
			return dev::server::RequestError{"invalid_request",
				"command " + std::to_string(command) + " field " + field + " " + requirement,
				{{"command", command}, {"field", field}}};
		};
		for (const auto& [name, fieldValue] : data.items()) {
			if (!fields.contains(name)) return invalid(name, "is not supported");
		}

		if (!data.contains("addr") || !IsAddress(data["addr"]))
			return invalid("addr", "must be an integer in the range 0..65535");
		uint64_t memPages = 0;
		if (!data.contains("memPages") || !ReadUnsigned(data["memPages"], memPages) ||
			memPages == 0 || memPages > dev::Breakpoint::MAPPING_PAGES_ALL)
			return invalid("memPages", "must be a non-zero 33-bit mapping mask");
		if (!data.contains("status") || !data["status"].is_string() ||
			(data["status"] != "ACTIVE" && data["status"] != "DISABLED"))
			return invalid("status", "must be ACTIVE or DISABLED");
		if (!data.contains("autoDelete") || !data["autoDelete"].is_boolean())
			return invalid("autoDelete", "must be boolean");

		const std::unordered_set<std::string> operands = {
			"A", "F", "B", "C", "D", "E", "H", "L", "PSW", "BC", "DE", "HL", "CC", "SP"
		};
		if (!data.contains("operand") || !data["operand"].is_string() || !operands.contains(data["operand"]))
			return invalid("operand", "has an unsupported value");
		if (!data.contains("condition") || !data["condition"].is_string() ||
			dev::ParseConditionName(data["condition"].get_ref<const std::string&>()) == dev::Condition::INVALID)
			return invalid("condition", "has an unsupported value");

		uint64_t value = 0;
		if (!data.contains("value") || !ReadUnsigned(data["value"], value))
			return invalid("value", "must be an unsigned integer");
		const auto& operand = data["operand"].get_ref<const std::string&>();
		const bool byteOperand = operand.size() == 1 && operand != "F" ? true : operand == "F";
		if (byteOperand && value > 0xFF) return invalid("value", "must fit the selected 8-bit operand");
		if (operand != "CC" && !byteOperand && value > 0xFFFF)
			return invalid("value", "must fit the selected 16-bit operand");
		if (!data.contains("comment") || !data["comment"].is_string() ||
			data["comment"].get_ref<const std::string&>().size() > MAX_COMMENT_BYTES ||
			!IsValidUtf8(data["comment"].get_ref<const std::string&>()))
			return invalid("comment", "must be a UTF-8 string of at most 1024 bytes");
		return std::nullopt;
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

	if (command == static_cast<int>(dev::Hardware::Req::GET_MEM)) {
		auto addressIt = data.find("addr");
		auto lengthIt = data.find("len");
		if (addressIt == data.end() || lengthIt == data.end() ||
			!addressIt->is_number_unsigned() || !lengthIt->is_number_unsigned()) {
			return RequestError{"invalid_request", "GET_MEM requires unsigned addr and len"};
		}

		const auto addr = addressIt->get<uint64_t>();
		const auto len = lengthIt->get<uint64_t>();
		if (len == 0 || addr >= dev::Memory::MEMORY_GLOBAL_LEN ||
			len > dev::Memory::MEMORY_GLOBAL_LEN - addr) {
			return RequestError{"invalid_request", "GET_MEM range must be non-empty and inside global memory"};
		}
	}

	if (command == static_cast<int>(dev::Hardware::Req::DEBUG_WATCHPOINT_ADD)) {
		if (auto error = ValidateStructuredWatchpoint(data, command)) return *error;
	}
	if (command == static_cast<int>(dev::Hardware::Req::DEBUG_WATCHPOINT_EDIT)) {
		if (auto error = ValidateStructuredWatchpoint(data, command, true)) return *error;
	}

	if (command == static_cast<int>(dev::Hardware::Req::DEBUG_WATCHPOINT_GET_ALL) && !data.empty()) {
		return RequestError{"invalid_request", "command 73 does not accept data"};
	}

	if (command == static_cast<int>(dev::Hardware::Req::DEBUG_MEMORY_EDIT_ADD)) {
		if (auto error = ValidateMemoryEditInput(data, command)) return *error;
	}
	if (command == static_cast<int>(dev::Hardware::Req::DEBUG_MEMORY_EDIT_DEL) ||
		command == static_cast<int>(dev::Hardware::Req::DEBUG_MEMORY_EDIT_GET) ||
		command == static_cast<int>(dev::Hardware::Req::DEBUG_MEMORY_EDIT_EXISTS) ||
		command == static_cast<int>(dev::Hardware::Req::DEBUG_MEMORY_EDIT_RESTORE)) {
		if (auto error = ValidateMemoryEditAddress(data, command)) return *error;
	}
	if ((command == static_cast<int>(dev::Hardware::Req::DEBUG_MEMORY_EDIT_DEL_ALL) ||
		command == static_cast<int>(dev::Hardware::Req::DEBUG_MEMORY_EDIT_GET_ALL)) && !data.empty()) {
		return RequestError{"invalid_request", "command " + std::to_string(command) + " does not accept data",
			{{"command", command}, {"field", data.items().begin().key()}}};
	}

	if (command == static_cast<int>(dev::Hardware::Req::DEBUG_CODE_PERF_ADD)) {
		if (auto error = ValidateCodePerfInput(data, command)) return *error;
	}
	if (command == static_cast<int>(dev::Hardware::Req::DEBUG_CODE_PERF_EDIT)) {
		if (auto error = ValidateCodePerfInput(data, command, true)) return *error;
	}
	if (command == static_cast<int>(dev::Hardware::Req::DEBUG_CODE_PERF_DEL) ||
		command == static_cast<int>(dev::Hardware::Req::DEBUG_CODE_PERF_GET) ||
		command == static_cast<int>(dev::Hardware::Req::DEBUG_CODE_PERF_EXISTS)) {
		if (auto error = ValidateCodePerfId(data, command)) return *error;
	}
	if ((command == static_cast<int>(dev::Hardware::Req::DEBUG_CODE_PERF_DEL_ALL) ||
		command == static_cast<int>(dev::Hardware::Req::DEBUG_CODE_PERF_GET_ALL)) && !data.empty()) {
		return RequestError{"invalid_request", "command " + std::to_string(command) + " does not accept data",
			{{"command", command}, {"field", data.items().begin().key()}}};
	}

	if (command == static_cast<int>(dev::Hardware::Req::DEBUG_BREAKPOINT_ADD)) {
		if (auto error = ValidateStructuredBreakpoint(data, command)) return *error;
	}
	if (command == static_cast<int>(dev::Hardware::Req::DEBUG_BREAKPOINT_DEL) ||
		command == static_cast<int>(dev::Hardware::Req::DEBUG_BREAKPOINT_GET_STATUS) ||
		command == static_cast<int>(dev::Hardware::Req::DEBUG_BREAKPOINT_ACTIVE) ||
		command == static_cast<int>(dev::Hardware::Req::DEBUG_BREAKPOINT_DISABLE)) {
		if (auto error = ValidateBreakpointAddress(data, command)) return *error;
	}
	if (command == static_cast<int>(dev::Hardware::Req::DEBUG_BREAKPOINT_SET_STATUS)) {
		if (auto error = ValidateBreakpointAddress(data, command, true)) return *error;
	}
	if ((command == static_cast<int>(dev::Hardware::Req::DEBUG_BREAKPOINT_DEL_ALL) ||
		command == static_cast<int>(dev::Hardware::Req::DEBUG_BREAKPOINT_GET_ALL) ||
		command == static_cast<int>(dev::Hardware::Req::DEBUG_BREAKPOINT_GET_UPDATES) ||
		command == static_cast<int>(dev::Hardware::Req::GET_STOP_RECORD)) && !data.empty()) {
		return RequestError{"invalid_request", "command " + std::to_string(command) + " does not accept data"};
	}

	if (command == static_cast<int>(dev::Hardware::Req::GET_HARDWARE_STATS) && !data.empty()) {
		return RequestError{"invalid_request", "GET_HARDWARE_STATS does not accept data"};
	}
	if (command == static_cast<int>(dev::Hardware::Req::SET_IO_PALETTE_ENTRY)) {
		uint64_t index = 0;
		uint64_t hwColor = 0;
		if (data.size() != 2 || !data.contains("index") || !ReadUnsigned(data["index"], index) || index > 15) {
			return RequestError{"invalid_request", "SET_IO_PALETTE_ENTRY index must be an integer in the range 0..15",
				{{"command", command}, {"field", "index"}}};
		}
		if (!data.contains("hwColor") || !ReadUnsigned(data["hwColor"], hwColor) || hwColor > 255) {
			return RequestError{"invalid_request", "SET_IO_PALETTE_ENTRY hwColor must be an integer in the range 0..255",
				{{"command", command}, {"field", "hwColor"}}};
		}
	}
	if (command == static_cast<int>(dev::Hardware::Req::DISMOUNT_FDD)) {
		uint64_t driveIdx = 0;
		if (data.size() != 1 || !data.contains("driveIdx") || !ReadUnsigned(data["driveIdx"], driveIdx) || driveIdx > 3) {
			return RequestError{"invalid_request", "DISMOUNT_FDD driveIdx must be an integer in the range 0..3",
				{{"command", command}, {"field", "driveIdx"}}};
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
		command <= static_cast<int>(dev::Hardware::Req::DEBUG_CODE_PERF_EDIT); ++command) {
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
			{"stackSampleSchema", 1},
			{"breakpointSchema", 1},
			{"watchpointSchema", 1},
			{"memoryEditSchema", 1},
			{"codePerfSchema", 1},
			{"stopRecordSchema", 1},
			{"hardwareStatsSchema", 1},
			{"hardwareStatsWhileRunning", true},
			{"paletteEntryMutation", true},
			{"fddDismount", true},
			{"runningHardwareMutations", false},
			{"breakpointLimits", {
				{"mappingPageBits", 33},
				{"maxCommentBytes", 1024}
			}},
			{"watchpointServerAllocatedIds", true},
			{"watchpointEdit", true},
			{"codePerfServerAllocatedIds", true},
			{"codePerfEdit", true},
			{"codePerfMutationsWhileRunning", true},
			{"watchpointMutationsWhileRunning", true},
			{"watchpointLimits", {
				{"maxRangeLength", dev::Memory::MEMORY_GLOBAL_LEN},
				{"maxCommentBytes", 1024}
			}},
			{"memoryEditLimits", {
				{"globalAddressExclusive", dev::Memory::MEMORY_GLOBAL_LEN},
				{"maxCommentBytes", 1024}
			}},
			{"codePerfLimits", {
				{"addressExclusive", 65536},
				{"maxNameBytes", dev::CodePerf::MAX_NAME_BYTES},
				{"maxRecords", dev::CodePerf::MAX_RECORDS},
				{"maxTestCount", dev::CodePerf::MAX_TEST_COUNT}
			}}
		}}
	};
}