#pragma once

// IPC command/response helpers.
// The wire protocol uses the same Req enum values as Hardware::Request().
// This header provides constants and helpers for the IPC layer.

#include <cstdint>

namespace dev::ipc
{
	// Wire‐format field names
	inline constexpr const char* FIELD_CMD   = "cmd";
	inline constexpr const char* FIELD_DATA  = "data";
	inline constexpr const char* FIELD_OK    = "ok";
	inline constexpr const char* FIELD_ERROR = "error";

	// Special pseudo-commands (negative values, not in the Req enum)
	inline constexpr int CMD_PING = -1;
	inline constexpr int CMD_PONG = -2;
	inline constexpr int CMD_GET_FRAME = -3; // fetch raw ABGR frame buffer
	inline constexpr int CMD_GET_FRAME_RAW = -4; // raw binary frame (no msgpack)
    // Hardware::Req::GET_HW_MAIN_STATS (positive cmd values map to Req enum)
	inline constexpr int CMD_GET_HW_MAIN_STATS = 43;
} // namespace dev::ipc
