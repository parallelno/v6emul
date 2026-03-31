#include "ipc/protocol.h"
#include "ipc/commands.h"

#include <cstring>

namespace dev::ipc
{

auto Encode(const nlohmann::json& _j) -> std::vector<uint8_t>
{
	auto msgpack = nlohmann::json::to_msgpack(_j);
	uint32_t len = static_cast<uint32_t>(msgpack.size());

	std::vector<uint8_t> result(4 + msgpack.size());
	std::memcpy(result.data(), &len, 4);  // little-endian
	std::memcpy(result.data() + 4, msgpack.data(), msgpack.size());
	return result;
}

auto Decode(const std::vector<uint8_t>& _payload) -> nlohmann::json
{
	return nlohmann::json::from_msgpack(_payload);
}

auto MakeResponse(const nlohmann::json& _data) -> nlohmann::json
{
	return {
		{FIELD_OK, true},
		{FIELD_DATA, _data}
	};
}

auto MakeErrorResponse(const std::string& _error) -> nlohmann::json
{
	return {
		{FIELD_OK, false},
		{FIELD_ERROR, _error}
	};
}

} // namespace dev::ipc
