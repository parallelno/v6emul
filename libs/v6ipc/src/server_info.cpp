#include "ipc/server_info.h"

#include <algorithm>

#include "ipc/commands.h"
#include "ipc/raw_frame.h"

auto dev::ipc::IsRawFrameServerCompatible(const nlohmann::json& response) -> bool
{
	try {
		if (!response.is_object() || !response.value(FIELD_OK, false)) return false;

		auto dataIt = response.find(FIELD_DATA);
		if (dataIt == response.end() || !dataIt->is_object() ||
			dataIt->value("protocolVersion", 0) != PROTOCOL_VERSION) {
			return false;
		}

		auto capabilitiesIt = dataIt->find("capabilities");
		if (capabilitiesIt == dataIt->end() || !capabilitiesIt->is_object() ||
			capabilitiesIt->value("rawFrameSchema", 0) != RAW_FRAME_SCHEMA_VERSION) {
			return false;
		}

		auto commandsIt = dataIt->find("commands");
		return commandsIt != dataIt->end() && commandsIt->is_array() &&
			std::find(commandsIt->begin(), commandsIt->end(), CMD_GET_FRAME_RAW) !=
			commandsIt->end();
	} catch (const nlohmann::json::exception&) {
		return false;
	}
}