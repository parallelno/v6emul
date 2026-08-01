#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace dev::ipc
{
	inline constexpr size_t RAW_FRAME_HEADER_SIZE = 16;
	inline constexpr uint8_t RAW_FRAME_SCHEMA_VERSION = 1;

	enum class RawFrameKind : uint8_t {
		FRAME = 1,
		ERROR_RESPONSE = 2,
	};

	enum class RawFrameErrorCode : uint32_t {
		FRAME_UNAVAILABLE = 1,
		INTERNAL_ERROR = 2,
	};

	struct RawFrameHeader {
		RawFrameKind kind;
		uint32_t value0;
		uint32_t value1;
	};

	auto EncodeRawFrame(std::vector<uint8_t>& message, uint32_t width,
		uint32_t height, const uint8_t* pixels, size_t pixelBytes) -> bool;
	auto EncodeRawFrameError(std::vector<uint8_t>& message,
		RawFrameErrorCode code, const std::string& errorMessage) -> bool;
	auto DecodeRawFrameHeader(std::span<const uint8_t> bytes)
		-> std::optional<RawFrameHeader>;
}