#pragma once

// Message framing: length-prefixed MessagePack via nlohmann::json.
//
// Wire format:
//   [4 bytes: uint32_t payload length, little-endian] [N bytes: MessagePack payload]
//
// Payload is nlohmann::json serialized with json::to_msgpack().
// Request:  { "cmd": <int>, "data": { ... } }
// Response: { "ok": true|false, "data": { ... }, "error": "..." }

#include <vector>
#include <cstdint>
#include <nlohmann/json.hpp>

namespace dev::ipc
{
	// Serialize a json object to a length-prefixed MessagePack message.
	// Returns bytes ready to send over the wire.
	auto Encode(const nlohmann::json& _j) -> std::vector<uint8_t>;

	// Deserialize a MessagePack payload (without the length prefix) back to json.
	auto Decode(const std::vector<uint8_t>& _payload) -> nlohmann::json;

	// Build a success response json.
	auto MakeResponse(const nlohmann::json& _data) -> nlohmann::json;

	// Build an error response json.
	auto MakeErrorResponse(const std::string& _error) -> nlohmann::json;

} // namespace dev::ipc
