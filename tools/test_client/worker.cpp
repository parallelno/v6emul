#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <WinSock2.h>
#include <Windows.h>

#include <cstring>
#include <thread>
#include <nlohmann/json.hpp>

#include "ipc/protocol.h"
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
			if (!ConnectToServer()) {
				Sleep(500);
				nextFrameTime = clock::now();
				continue;
			}
		}

		// Every ~1 second, request stats
		DWORD now = GetTickCount();
		if (now - lastStatsTick >= 1000) {
			lastStatsTick = now;

			if (!SendExact(g_statsRequestBytes.data(), g_statsRequestBytes.size())) {
				Disconnect(); continue;
			}
			uint32_t msgLen = 0;
			if (!RecvExact(&msgLen, 4)) { Disconnect(); continue; }
			if (msgLen == 0 || msgLen > 64 * 1024 * 1024) { Disconnect(); continue; }
			std::vector<uint8_t> msgBuf(msgLen);
			if (!RecvExact(msgBuf.data(), msgLen)) { Disconnect(); continue; }
			try {
				auto respJ = nlohmann::json::from_msgpack(msgBuf);
				if (respJ.contains("data") && respJ["data"].contains("speedPercent"))
					g_speedPercent.store(static_cast<int>(respJ["data"]["speedPercent"].get<double>()));
			} catch (...) {}
		}

		if (!FlushKeyEvents()) { Disconnect(); continue; }

		// Send frame request
		if (!SendExact(g_requestBytes.data(), g_requestBytes.size())) {
			Disconnect(); continue;
		}

		// Read header: [4:payloadLen][4:width][4:height]
		uint32_t payloadLen = 0;
		if (!RecvExact(&payloadLen, 4)) { Disconnect(); continue; }
		if (payloadLen < 8 || payloadLen > 64 * 1024 * 1024) { Disconnect(); continue; }

		uint32_t width = 0, height = 0;
		if (!RecvExact(&width,  4)) { Disconnect(); continue; }
		if (!RecvExact(&height, 4)) { Disconnect(); continue; }

		size_t pixelBytes = payloadLen - 8;
		if (width == 0 || height == 0
			|| width  > static_cast<uint32_t>(MAX_FRAME_W)
			|| height > static_cast<uint32_t>(MAX_FRAME_H)
			|| pixelBytes != static_cast<size_t>(width) * height * 4) {
			Disconnect(); continue;
		}

		if (!RecvExact(g_frameBack.data(), pixelBytes)) { Disconnect(); continue; }

		g_frameW.store(static_cast<int>(width));
		g_frameH.store(static_cast<int>(height));
		{
			std::lock_guard lock(g_frameMutex);
			g_frameFront.swap(g_frameBack);
		}
		g_frameReady.store(true);
		g_recvCount.fetch_add(1);

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
