#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <WinSock2.h>
#include <Windows.h>

#include <cstring>
#include <array>
#include <optional>
#include <thread>
#include <nlohmann/json.hpp>

#include "ipc/protocol.h"
#include "ipc/raw_frame.h"
#include "ipc/server_info.h"
#include "ipc/commands.h"
#include "core/hardware_consts.h"
#include "net.h"
#include "worker.h"

// ── Global definitions ────────────────────────────────────────────────
std::vector<uint32_t> g_frameFront(MAX_FRAME_PIXELS, 0);
std::vector<uint32_t> g_frameBack(MAX_FRAME_PIXELS, 0);
std::mutex            g_frameMutex;
std::atomic<bool>     g_frameReady{false};
std::atomic<int>      g_frameW{0};
std::atomic<int>      g_frameH{0};

std::atomic<bool>     g_running{true};
bool                  g_testLocal = false;

std::atomic<int>      g_recvCount{0};
std::atomic<int>      g_speedPercent{0};

std::vector<uint8_t>  g_requestBytes;
std::vector<uint8_t>  g_statsRequestBytes;

std::vector<KeyEvent> g_keyQueue;
std::mutex            g_keyMutex;

static auto ReceiveMessagePack() -> std::optional<nlohmann::json>
{
	uint32_t payloadLen = 0;
	if (!RecvExact(&payloadLen, 4) || payloadLen == 0 || payloadLen > 64 * 1024 * 1024) {
		return std::nullopt;
	}

	std::vector<uint8_t> payload(payloadLen);
	if (!RecvExact(payload.data(), payload.size())) return std::nullopt;
	try {
		return dev::ipc::Decode(payload);
	} catch (...) {
		return std::nullopt;
	}
}

static bool NegotiateProtocol()
{
	auto request = dev::ipc::Encode({
		{dev::ipc::FIELD_CMD, dev::ipc::CMD_GET_SERVER_INFO},
		{dev::ipc::FIELD_DATA, nlohmann::json::object()}
	});
	if (!SendExact(request.data(), request.size())) return false;

	auto response = ReceiveMessagePack();
	return response && dev::ipc::IsRawFrameServerCompatible(*response);
}

// ── Send queued key events ────────────────────────────────────────────
static bool FlushKeyEvents()
{
	std::vector<KeyEvent> events;
	{
		std::lock_guard lock(g_keyMutex);
		events.swap(g_keyQueue);
	}
	for (const auto& ev : events) {
		nlohmann::json keyReq = {
			{dev::ipc::FIELD_CMD, static_cast<int>(Req::KEY_HANDLING)},
			{dev::ipc::FIELD_DATA, {{"scancode", ev.keyCode}, {"action", ev.action}}}
		};
		auto encoded = dev::ipc::Encode(keyReq);
		if (!SendExact(encoded.data(), encoded.size())) return false;

		uint32_t respLen = 0;
		if (!RecvExact(&respLen, 4)) return false;
		if (respLen > 64 * 1024 * 1024) return false;
		std::vector<uint8_t> respBuf(respLen);
		if (!RecvExact(respBuf.data(), respLen)) return false;
	}
	return true;
}

// ── Worker thread: fetch frames at 50 fps ────────────────────────────
void WorkerThread()
{
	using clock = std::chrono::steady_clock;

	DWORD lastStatsTick = GetTickCount();
	auto nextFrameTime  = clock::now();

	while (g_running.load()) {

		if (g_testLocal) {
			// Diagnostic: generate a solid-color frame locally
			g_frameW.store(MAX_FRAME_W);
			g_frameH.store(MAX_FRAME_H);
			memset(g_frameBack.data(), rand() & 0xFF, MAX_FRAME_BYTES);
			{
				std::lock_guard lock(g_frameMutex);
				g_frameFront.swap(g_frameBack);
			}
			g_frameReady.store(true);
			g_recvCount.fetch_add(1);

			nextFrameTime += FRAME_INTERVAL;
			auto clockNow = clock::now();
			if (nextFrameTime > clockNow) {
				auto remaining = nextFrameTime - clockNow;
				auto sleepPart = remaining - std::chrono::milliseconds(2);
				if (sleepPart > std::chrono::milliseconds(0))
					std::this_thread::sleep_for(sleepPart);
				while (clock::now() < nextFrameTime)
					;
			} else {
				nextFrameTime = clockNow;
			}
			continue;
		}

		if (!g_connected.load()) {
			if (!ConnectToServer() || !NegotiateProtocol()) {
				Disconnect();
				Sleep(500);
				nextFrameTime = clock::now();
				continue;
			}
			g_connected.store(true);
		}

		// Every ~1 second, request stats
		DWORD now = GetTickCount();
		if (now - lastStatsTick >= 1000) {
			lastStatsTick = now;

			if (!SendExact(g_statsRequestBytes.data(), g_statsRequestBytes.size())) {
				Disconnect(); continue;
			}
			auto response = ReceiveMessagePack();
			if (!response) { Disconnect(); continue; }
			auto dataIt = response->find(dev::ipc::FIELD_DATA);
			if (dataIt != response->end() && dataIt->is_object() && dataIt->contains("speedPercent")) {
				g_speedPercent.store(static_cast<int>((*dataIt)["speedPercent"].get<double>()));
			}
		}

		if (!FlushKeyEvents()) { Disconnect(); continue; }

		// Send frame request
		if (!SendExact(g_requestBytes.data(), g_requestBytes.size())) {
			Disconnect(); continue;
		}

		// Read the fixed raw-frame envelope.
		uint32_t payloadLen = 0;
		if (!RecvExact(&payloadLen, 4)) { Disconnect(); continue; }
		if (payloadLen < dev::ipc::RAW_FRAME_HEADER_SIZE || payloadLen > 64 * 1024 * 1024) {
			Disconnect(); continue;
		}

		std::array<uint8_t, dev::ipc::RAW_FRAME_HEADER_SIZE> headerBytes{};
		if (!RecvExact(headerBytes.data(), headerBytes.size())) { Disconnect(); continue; }
		auto header = dev::ipc::DecodeRawFrameHeader(headerBytes);
		if (!header) { Disconnect(); continue; }

		const size_t bodyBytes = payloadLen - dev::ipc::RAW_FRAME_HEADER_SIZE;
		if (header->kind == dev::ipc::RawFrameKind::ERROR_RESPONSE) {
			if (header->value1 != bodyBytes) { Disconnect(); continue; }
			std::vector<uint8_t> errorMessage(bodyBytes);
			if (!RecvExact(errorMessage.data(), errorMessage.size())) { Disconnect(); continue; }
		} else {
			const uint32_t width = header->value0;
			const uint32_t height = header->value1;
			if (width == 0 || height == 0
				|| width > static_cast<uint32_t>(MAX_FRAME_W)
				|| height > static_cast<uint32_t>(MAX_FRAME_H)
				|| bodyBytes != static_cast<size_t>(width) * height * 4) {
				Disconnect(); continue;
			}

			if (!RecvExact(g_frameBack.data(), bodyBytes)) { Disconnect(); continue; }

			g_frameW.store(static_cast<int>(width));
			g_frameH.store(static_cast<int>(height));
			{
				std::lock_guard lock(g_frameMutex);
				g_frameFront.swap(g_frameBack);
			}
			g_frameReady.store(true);
			g_recvCount.fetch_add(1);
		}

		// Pace to 50 fps using absolute time tracking.
		// Hybrid sleep+spin: sleep most of the interval, then spin the last ~2ms.
		nextFrameTime += FRAME_INTERVAL;
		auto clockNow = clock::now();
		if (nextFrameTime > clockNow) {
			auto remaining = nextFrameTime - clockNow;
			auto sleepPart = remaining - std::chrono::milliseconds(2);
			if (sleepPart > std::chrono::milliseconds(0))
				std::this_thread::sleep_for(sleepPart);
			while (clock::now() < nextFrameTime)
				;
		} else {
			// Fell behind — reset to avoid burst catch-up
			nextFrameTime = clockNow;
		}
	}
}
