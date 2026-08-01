#include "ipc/raw_frame.h"

#include <cstring>
#include <limits>

namespace
{
	constexpr uint8_t MAGIC[] = {'V', '6', 'R', 'F'};

	void WriteU32(std::vector<uint8_t>& out, const size_t offset, const uint32_t value)
	{
		out[offset] = static_cast<uint8_t>(value);
		out[offset + 1] = static_cast<uint8_t>(value >> 8);
		out[offset + 2] = static_cast<uint8_t>(value >> 16);
		out[offset + 3] = static_cast<uint8_t>(value >> 24);
	}

	auto ReadU32(const std::span<const uint8_t> bytes, const size_t offset) -> uint32_t
	{
		return static_cast<uint32_t>(bytes[offset]) |
			(static_cast<uint32_t>(bytes[offset + 1]) << 8) |
			(static_cast<uint32_t>(bytes[offset + 2]) << 16) |
			(static_cast<uint32_t>(bytes[offset + 3]) << 24);
	}

	auto MakeMessage(std::vector<uint8_t>& message, const dev::ipc::RawFrameKind kind,
		const uint32_t value0, const uint32_t value1, const uint8_t* body,
		const size_t bodySize) -> bool
	{
		if (bodySize > std::numeric_limits<uint32_t>::max() - dev::ipc::RAW_FRAME_HEADER_SIZE) {
			message.clear();
			return false;
		}

		const auto payloadSize = static_cast<uint32_t>(dev::ipc::RAW_FRAME_HEADER_SIZE + bodySize);
		message.resize(4 + payloadSize);
		WriteU32(message, 0, payloadSize);
		std::memcpy(message.data() + 4, MAGIC, sizeof(MAGIC));
		message[8] = dev::ipc::RAW_FRAME_SCHEMA_VERSION;
		message[9] = static_cast<uint8_t>(kind);
		message[10] = 0;
		message[11] = 0;
		WriteU32(message, 12, value0);
		WriteU32(message, 16, value1);
		if (bodySize > 0) std::memcpy(message.data() + 20, body, bodySize);
		return true;
	}
}

auto dev::ipc::EncodeRawFrame(std::vector<uint8_t>& message, const uint32_t width,
	const uint32_t height, const uint8_t* pixels, const size_t pixelBytes) -> bool
{
	if (pixels == nullptr && pixelBytes > 0) {
		message.clear();
		return false;
	}
	return MakeMessage(message, RawFrameKind::FRAME, width, height, pixels, pixelBytes);
}

auto dev::ipc::EncodeRawFrameError(std::vector<uint8_t>& message,
	const RawFrameErrorCode code, const std::string& errorMessage) -> bool
{
	return MakeMessage(message, RawFrameKind::ERROR_RESPONSE, static_cast<uint32_t>(code),
		static_cast<uint32_t>(errorMessage.size()),
		reinterpret_cast<const uint8_t*>(errorMessage.data()), errorMessage.size());
}

auto dev::ipc::DecodeRawFrameHeader(const std::span<const uint8_t> bytes)
	-> std::optional<RawFrameHeader>
{
	if (bytes.size() != RAW_FRAME_HEADER_SIZE ||
		!std::equal(std::begin(MAGIC), std::end(MAGIC), bytes.begin()) ||
		bytes[4] != RAW_FRAME_SCHEMA_VERSION || bytes[6] != 0 || bytes[7] != 0) {
		return std::nullopt;
	}

	auto kind = static_cast<RawFrameKind>(bytes[5]);
	if (kind != RawFrameKind::FRAME && kind != RawFrameKind::ERROR_RESPONSE) return std::nullopt;
	return RawFrameHeader{kind, ReadU32(bytes, 8), ReadU32(bytes, 12)};
}