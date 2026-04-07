// Test client: connects to the emulator IPC server, fetches frames at 50fps,
// and displays them in a Win32 GDI window.
//
// Usage: test_client.exe [--port 9876]

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <WinSock2.h>
#include <WS2tcpip.h>
#include <Windows.h>
#include <shellapi.h>
#include <timeapi.h>
#pragma comment(lib, "ws2_32.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "winmm.lib")

#include <cstdint>
#include <cstring>
#include <vector>
#include <string>
#include <format>
#include <thread>
#include <chrono>
#include <atomic>
#include <mutex>

#include <nlohmann/json.hpp>
#include "ipc/protocol.h"
#include "ipc/commands.h"
#include "core/hardware_consts.h"

// ── KeyCode enum: must mirror dev::KeyCode from core/keyboard.h ──────
// We duplicate this to avoid linking v6core into the test client.
namespace KeyCode {
	enum : int {
		A = 0, B, C, D, E, F, G, H, I, J, K, L, M, N, O, P, Q, R, S, T, U, V, W, X, Y, Z,
		NUM_0, NUM_1, NUM_2, NUM_3, NUM_4, NUM_5, NUM_6, NUM_7, NUM_8, NUM_9,
		SPACE, MINUS, EQUALS, LEFTBRACKET, RIGHTBRACKET, BACKSLASH,
		SEMICOLON, APOSTROPHE, GRAVE, COMMA, PERIOD, SLASH,
		F1, F2, F3, F4, F5, F6, F7, F8,
		TAB, RETURN, BACKSPACE, ESCAPE,
		UP, DOWN, LEFT, RIGHT,
		LSHIFT, RSHIFT, LCTRL, LGUI, LALT, RALT,
		F11, F12,
		COUNT,
		INVALID = -1
	};
}

static constexpr int KEY_ACTION_UP = 0;
static constexpr int KEY_ACTION_DOWN = 1;

// Map Win32 virtual key codes to KeyCode values
static int VkToKeyCode(WPARAM vk)
{
	switch (vk) {
	case 'A': return KeyCode::A;  case 'B': return KeyCode::B;
	case 'C': return KeyCode::C;  case 'D': return KeyCode::D;
	case 'E': return KeyCode::E;  case 'F': return KeyCode::F;
	case 'G': return KeyCode::G;  case 'H': return KeyCode::H;
	case 'I': return KeyCode::I;  case 'J': return KeyCode::J;
	case 'K': return KeyCode::K;  case 'L': return KeyCode::L;
	case 'M': return KeyCode::M;  case 'N': return KeyCode::N;
	case 'O': return KeyCode::O;  case 'P': return KeyCode::P;
	case 'Q': return KeyCode::Q;  case 'R': return KeyCode::R;
	case 'S': return KeyCode::S;  case 'T': return KeyCode::T;
	case 'U': return KeyCode::U;  case 'V': return KeyCode::V;
	case 'W': return KeyCode::W;  case 'X': return KeyCode::X;
	case 'Y': return KeyCode::Y;  case 'Z': return KeyCode::Z;
	case '0': return KeyCode::NUM_0;  case '1': return KeyCode::NUM_1;
	case '2': return KeyCode::NUM_2;  case '3': return KeyCode::NUM_3;
	case '4': return KeyCode::NUM_4;  case '5': return KeyCode::NUM_5;
	case '6': return KeyCode::NUM_6;  case '7': return KeyCode::NUM_7;
	case '8': return KeyCode::NUM_8;  case '9': return KeyCode::NUM_9;
	case VK_SPACE:   return KeyCode::SPACE;
	case VK_OEM_MINUS:  return KeyCode::MINUS;
	case VK_OEM_PLUS:   return KeyCode::EQUALS;
	case VK_OEM_4:   return KeyCode::LEFTBRACKET;   // [
	case VK_OEM_6:   return KeyCode::RIGHTBRACKET;  // ]
	case VK_OEM_5:   return KeyCode::BACKSLASH;     // '\'
	case VK_OEM_1:   return KeyCode::SEMICOLON;     // ;
	case VK_OEM_7:   return KeyCode::APOSTROPHE;    // '
	case VK_OEM_3:   return KeyCode::GRAVE;         // `
	case VK_OEM_COMMA:  return KeyCode::COMMA;
	case VK_OEM_PERIOD: return KeyCode::PERIOD;
	case VK_OEM_2:   return KeyCode::SLASH;         // /
	case VK_F1: return KeyCode::F1;  case VK_F2: return KeyCode::F2;
	case VK_F3: return KeyCode::F3;  case VK_F4: return KeyCode::F4;
	case VK_F5: return KeyCode::F5;  case VK_F6: return KeyCode::F6;
	case VK_F7: return KeyCode::F7;  case VK_F8: return KeyCode::F8;
	case VK_F11: return KeyCode::F11;
	case VK_F12: return KeyCode::F12;
	case VK_TAB:     return KeyCode::TAB;
	case VK_RETURN:  return KeyCode::RETURN;
	case VK_BACK:    return KeyCode::BACKSPACE;
	case VK_ESCAPE:  return KeyCode::ESCAPE;
	case VK_UP:      return KeyCode::UP;
	case VK_DOWN:    return KeyCode::DOWN;
	case VK_LEFT:    return KeyCode::LEFT;
	case VK_RIGHT:   return KeyCode::RIGHT;
	case VK_LSHIFT:  return KeyCode::LSHIFT;
	case VK_RSHIFT:  return KeyCode::RSHIFT;
	case VK_LCONTROL: return KeyCode::LCTRL;
	case VK_LWIN:    return KeyCode::LGUI;
	case VK_LMENU:   return KeyCode::LALT;
	case VK_RMENU:   return KeyCode::RALT;
	default:         return KeyCode::INVALID;
	}
}

// Resolve left/right variants for shift, ctrl, alt
static WPARAM MapExtendedKey(WPARAM vk, LPARAM lParam)
{
	switch (vk) {
	case VK_SHIFT:   return MapVirtualKeyW((lParam >> 16) & 0xFF, MAPVK_VSC_TO_VK_EX);
	case VK_CONTROL: return (lParam & (1 << 24)) ? VK_RCONTROL : VK_LCONTROL;
	case VK_MENU:    return (lParam & (1 << 24)) ? VK_RMENU : VK_LMENU;
	default:         return vk;
	}
}


// ── Frame constants ───────────────────────────────────────────────────
// Maximum frame size (full mode); actual size is dynamic from server.
static constexpr int MAX_FRAME_W = 768;
static constexpr int MAX_FRAME_H = 312;
static constexpr int MAX_FRAME_PIXELS = MAX_FRAME_W * MAX_FRAME_H;
static constexpr int MAX_FRAME_BYTES = MAX_FRAME_PIXELS * 4;
// Scale factor for the window
static constexpr int SCALE = 1;

// 50 fps
static constexpr auto FRAME_INTERVAL = std::chrono::microseconds(20000);

// Timer ID for repaint
static constexpr UINT_PTR TIMER_ID = 1;
static constexpr UINT TIMER_MS = 10;

// ── Globals ───────────────────────────────────────────────────────────
static SOCKET g_sock = INVALID_SOCKET;
static uint16_t g_port = 9876;
static BITMAPINFOHEADER g_bmiHeader{};
static std::vector<uint8_t> g_requestBytes;
static std::vector<uint8_t> g_statsRequestBytes;

// Current frame dimensions (set by worker thread from server response)
static std::atomic<int> g_frameW{0};
static std::atomic<int> g_frameH{0};

// Double-buffered frames: worker writes to back, UI reads from front
static std::vector<uint32_t> g_frameFront(MAX_FRAME_PIXELS, 0);
static std::vector<uint32_t> g_frameBack(MAX_FRAME_PIXELS, 0);
static std::mutex g_frameMutex;
static std::atomic<bool> g_frameReady{false};

// Paint buffer: UI thread copies g_frameFront here under lock, then paints without lock
static std::vector<uint32_t> g_paintBuffer(MAX_FRAME_PIXELS, 0);

// Stats
static std::atomic<int> g_recvCount{0};
static std::atomic<bool> g_connected{false};
static std::atomic<int> g_speedPercent{0};

// Worker thread control
static std::atomic<bool> g_running{true};
static HWND g_hwnd = nullptr;

// Diagnostic: bypass server, generate local frames
static bool g_testLocal = false;

// Key event queue: UI thread pushes, worker thread drains and sends
struct KeyEvent { int keyCode; int action; };
static std::vector<KeyEvent> g_keyQueue;
static std::mutex g_keyMutex;

// ── Socket helpers ────────────────────────────────────────────────────
static bool RecvExact(void* buf, size_t len)
{
	auto* p = static_cast<char*>(buf);
	size_t remaining = len;
	while (remaining > 0) {
		int n = recv(g_sock, p, static_cast<int>(remaining), 0);
		if (n <= 0) return false;
		p += n;
		remaining -= n;
	}
	return true;
}

static bool SendExact(const void* buf, size_t len)
{
	auto* p = static_cast<const char*>(buf);
	size_t remaining = len;
	while (remaining > 0) {
		int n = send(g_sock, p, static_cast<int>(remaining), 0);
		if (n <= 0) return false;
		p += n;
		remaining -= n;
	}
	return true;
}

static bool ConnectToServer()
{
	if (g_sock != INVALID_SOCKET) {
		closesocket(g_sock);
		g_sock = INVALID_SOCKET;
	}

	g_sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
	if (g_sock == INVALID_SOCKET) return false;

	BOOL nodelay = TRUE;
	setsockopt(g_sock, IPPROTO_TCP, TCP_NODELAY,
		reinterpret_cast<const char*>(&nodelay), sizeof(nodelay));

	// Increase receive buffer to hold several frames
	int rcvBuf = 4 * 1024 * 1024;
	setsockopt(g_sock, SOL_SOCKET, SO_RCVBUF,
		reinterpret_cast<const char*>(&rcvBuf), sizeof(rcvBuf));

	sockaddr_in addr{};
	addr.sin_family = AF_INET;
	addr.sin_port = htons(g_port);
	inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);

	if (connect(g_sock, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == SOCKET_ERROR) {
		closesocket(g_sock);
		g_sock = INVALID_SOCKET;
		return false;
	}

	g_connected.store(true);
	return true;
}

static void Disconnect()
{
	if (g_sock != INVALID_SOCKET) {
		closesocket(g_sock);
		g_sock = INVALID_SOCKET;
	}
	g_connected.store(false);
}

// ── Send queued key events ─────────────────────────────────────────────
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

		// Consume the response (KEY_HANDLING returns empty data)
		uint32_t respLen = 0;
		if (!RecvExact(&respLen, 4)) return false;
		if (respLen > 64 * 1024 * 1024) return false;
		std::vector<uint8_t> respBuf(respLen);
		if (!RecvExact(respBuf.data(), respLen)) return false;
	}
	return true;
}

// ── Worker thread: fetch frames at 50 fps ─────────────────────────────
static void WorkerThread()
{
	using clock = std::chrono::steady_clock;


	DWORD lastStatsTick = GetTickCount();
	auto nextFrameTime = clock::now();

	while (g_running.load()) {

		if (g_testLocal) {
			// ── Diagnostic: generate a solid-color frame locally ──
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

		// Every ~1 second, request stats (inline, doesn't skip a frame)
		DWORD now = GetTickCount();
		if (now - lastStatsTick >= 1000) {
			lastStatsTick = now;

			if (!SendExact(g_statsRequestBytes.data(), g_statsRequestBytes.size())) {
				Disconnect();
				continue;
			}

			uint32_t msgLen = 0;
			if (!RecvExact(&msgLen, 4)) { Disconnect(); continue; }
			if (msgLen == 0 || msgLen > 64 * 1024 * 1024) { Disconnect(); continue; }

			std::vector<uint8_t> msgBuf(msgLen);
			if (!RecvExact(msgBuf.data(), msgLen)) { Disconnect(); continue; }

			try {
				auto respJ = nlohmann::json::from_msgpack(msgBuf);
				if (respJ.contains("data") && respJ["data"].contains("speedPercent")) {
					g_speedPercent.store(static_cast<int>(respJ["data"]["speedPercent"].get<double>()));
				}
			} catch (...) {}
			// fall through to fetch frame in the same iteration
		}

		// Send any queued key events
		if (!FlushKeyEvents()) {
			Disconnect();
			continue;
		}

		// Send frame request
		if (!SendExact(g_requestBytes.data(), g_requestBytes.size())) {
			Disconnect();
			continue;
		}

		// Read header: [4:payloadLen][4:width][4:height]
		uint32_t payloadLen = 0;
		if (!RecvExact(&payloadLen, 4)) { Disconnect(); continue; }
		if (payloadLen < 8 || payloadLen > 64 * 1024 * 1024) { Disconnect(); continue; }

		uint32_t width = 0, height = 0;
		if (!RecvExact(&width, 4)) { Disconnect(); continue; }
		if (!RecvExact(&height, 4)) { Disconnect(); continue; }

		size_t pixelBytes = payloadLen - 8;
		if (width == 0 || height == 0
			|| width > static_cast<uint32_t>(MAX_FRAME_W)
			|| height > static_cast<uint32_t>(MAX_FRAME_H)
			|| pixelBytes != static_cast<size_t>(width) * height * 4) {
			Disconnect();
			continue;
		}

		// Receive directly into back buffer
		if (!RecvExact(g_frameBack.data(), pixelBytes)) { Disconnect(); continue; }

		// Update dynamic frame dimensions
		g_frameW.store(static_cast<int>(width));
		g_frameH.store(static_cast<int>(height));

		// Swap to front under lock
		{
			std::lock_guard lock(g_frameMutex);
			g_frameFront.swap(g_frameBack);
		}
		g_frameReady.store(true);
		g_recvCount.fetch_add(1);

		// Pace to 50 fps using absolute time tracking.
		// Hybrid sleep+spin: sleep most of the interval, then spin-wait
		// the last ~2ms for precision. Pure sleep_for overshoots by 1-2ms
		// on Windows even with timeBeginPeriod(1).
		nextFrameTime += FRAME_INTERVAL;
		auto clockNow = clock::now();
		if (nextFrameTime > clockNow) {
			auto remaining = nextFrameTime - clockNow;
			auto sleepPart = remaining - std::chrono::milliseconds(2);
			if (sleepPart > std::chrono::milliseconds(0))
				std::this_thread::sleep_for(sleepPart);
			while (clock::now() < nextFrameTime)
				; // spin-wait for precision
		} else {
			// Fell behind (e.g. slow network) — reset to avoid burst catch-up
			nextFrameTime = clockNow;
		}
	}
}

// ── Window procedure ──────────────────────────────────────────────────
static int g_fps = 0;
static int g_frameCount = 0;
static DWORD g_lastFpsTick = 0;

static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
	switch (msg)
	{
	case WM_CREATE:
		SetTimer(hwnd, TIMER_ID, TIMER_MS, nullptr);
		g_lastFpsTick = GetTickCount();
		return 0;

	case WM_TIMER:
		if (wParam == TIMER_ID) {
			if (g_frameReady.exchange(false)) {
				g_frameCount++;
				InvalidateRect(hwnd, nullptr, FALSE);
			}
			DWORD now = GetTickCount();
			if (now - g_lastFpsTick >= 1000) {
				g_fps = g_recvCount.exchange(0);
				g_frameCount = 0;
				g_lastFpsTick = now;

				auto title = std::format(L"v6emul test client  |  {}x{}  |  {} fps  |  speed {}%  |  {}",
					g_frameW.load(), g_frameH.load(), g_fps, g_speedPercent.load(),
					g_connected.load() ? L"connected" : L"disconnected");
				SetWindowTextW(hwnd, title.c_str());
			}
		}
		return 0;

	case WM_PAINT:
	{
		PAINTSTRUCT ps;
		HDC hdc = BeginPaint(hwnd, &ps);

		RECT rc;
		GetClientRect(hwnd, &rc);
		int clientW = rc.right - rc.left;
		int clientH = rc.bottom - rc.top;

		int fw = g_frameW.load();
		int fh = g_frameH.load();
		if (fw <= 0 || fh <= 0) { EndPaint(hwnd, &ps); return 0; }

		// Copy frame out of lock so StretchDIBits doesn't block the worker
		size_t copyBytes = static_cast<size_t>(fw) * fh * 4;
		{
			std::lock_guard lock(g_frameMutex);
			memcpy(g_paintBuffer.data(), g_frameFront.data(), copyBytes);
		}

		// Update bitmap header for current frame dimensions
		g_bmiHeader.biWidth = fw;
		g_bmiHeader.biHeight = -fh;

		SetStretchBltMode(hdc, COLORONCOLOR);
		StretchDIBits(hdc,
			0, 0, clientW, clientH,
			0, 0, fw, fh,
			g_paintBuffer.data(), reinterpret_cast<const BITMAPINFO*>(&g_bmiHeader),
			DIB_RGB_COLORS, SRCCOPY);

		EndPaint(hwnd, &ps);
		return 0;
	}

	case WM_KEYDOWN:
	{
		WPARAM mapped = MapExtendedKey(wParam, lParam);
		int kc = VkToKeyCode(mapped);
		if (kc != KeyCode::INVALID) {
			std::lock_guard lock(g_keyMutex);
			g_keyQueue.push_back({kc, KEY_ACTION_DOWN});
		}
		return 0;
	}

	case WM_KEYUP:
	{
		WPARAM mapped = MapExtendedKey(wParam, lParam);
		int kc = VkToKeyCode(mapped);
		if (kc != KeyCode::INVALID) {
			std::lock_guard lock(g_keyMutex);
			g_keyQueue.push_back({kc, KEY_ACTION_UP});
		}
		// Escape on key-up closes the window
		if (mapped == VK_ESCAPE) {
			DestroyWindow(hwnd);
		}
		return 0;
	}

	case WM_SYSKEYDOWN:
	{
		// Intercept Alt/F10 so Windows doesn't activate the menu bar
		WPARAM mapped = MapExtendedKey(wParam, lParam);
		int kc = VkToKeyCode(mapped);
		if (kc != KeyCode::INVALID) {
			std::lock_guard lock(g_keyMutex);
			g_keyQueue.push_back({kc, KEY_ACTION_DOWN});
		}
		return 0;
	}

	case WM_SYSKEYUP:
	{
		WPARAM mapped = MapExtendedKey(wParam, lParam);
		int kc = VkToKeyCode(mapped);
		if (kc != KeyCode::INVALID) {
			std::lock_guard lock(g_keyMutex);
			g_keyQueue.push_back({kc, KEY_ACTION_UP});
		}
		return 0;
	}

	case WM_DESTROY:
		KillTimer(hwnd, TIMER_ID);
		g_running.store(false);
		Disconnect();
		PostQuitMessage(0);
		return 0;
	}

	return DefWindowProcW(hwnd, msg, wParam, lParam);
}

// ── Entry point ───────────────────────────────────────────────────────
int WINAPI wWinMain(HINSTANCE hInstance, HINSTANCE, LPWSTR lpCmdLine, int nCmdShow)
{
	// Parse --port from command line
	int argc;
	LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
	for (int i = 1; i < argc; ++i) {
		if (wcscmp(argv[i], L"--port") == 0 && i + 1 < argc) {
			g_port = static_cast<uint16_t>(_wtoi(argv[i + 1]));
			++i;
		}
		else if (wcscmp(argv[i], L"--test-local") == 0) {
			g_testLocal = true;
		}
	}
	LocalFree(argv);

	// Set system timer resolution to 1ms for accurate sleep_for timing.
	// Must be called before the worker thread starts.
	timeBeginPeriod(1);

	// Init Winsock
	WSADATA wsa;
	if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
		MessageBoxW(nullptr, L"WSAStartup failed", L"Error", MB_OK);
		return 1;
	}

	// Set up DIB info: BI_RGB for standard BGRA byte order (server converts)
	g_bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
	g_bmiHeader.biWidth = MAX_FRAME_W;
	g_bmiHeader.biHeight = -MAX_FRAME_H; // top-down
	g_bmiHeader.biPlanes = 1;
	g_bmiHeader.biBitCount = 32;
	g_bmiHeader.biCompression = BI_RGB;

	// Pre-encode the frame request (reused every frame)
	nlohmann::json reqJ = {
		{dev::ipc::FIELD_CMD, dev::ipc::CMD_GET_FRAME_RAW},
		{dev::ipc::FIELD_DATA, nullptr}
	};
	g_requestBytes = dev::ipc::Encode(reqJ);

	// Pre-encode the stats request
	{
		nlohmann::json statsReqJ = {
			{dev::ipc::FIELD_CMD, static_cast<int>(Req::GET_HW_MAIN_STATS)},
			{dev::ipc::FIELD_DATA, nullptr}
		};
		g_statsRequestBytes = dev::ipc::Encode(statsReqJ);
	}

	// Register window class
	WNDCLASSEXW wc{};
	wc.cbSize = sizeof(wc);
	wc.style = CS_HREDRAW | CS_VREDRAW;
	wc.lpfnWndProc = WndProc;
	wc.hInstance = hInstance;
	wc.hCursor = LoadCursorW(nullptr, MAKEINTRESOURCEW(32512));
	wc.hbrBackground = static_cast<HBRUSH>(GetStockObject(BLACK_BRUSH));
	wc.lpszClassName = L"V6TestClient";
	RegisterClassExW(&wc);

	// Calculate window size for desired client area
	RECT winRect = {0, 0, MAX_FRAME_W * SCALE, MAX_FRAME_H * SCALE};
	AdjustWindowRect(&winRect, WS_OVERLAPPEDWINDOW, FALSE);

	HWND hwnd = CreateWindowExW(0, L"V6TestClient",
		L"v6emul test client  |  connecting...",
		WS_OVERLAPPEDWINDOW,
		CW_USEDEFAULT, CW_USEDEFAULT,
		winRect.right - winRect.left, winRect.bottom - winRect.top,
		nullptr, nullptr, hInstance, nullptr);

	if (!hwnd) {
		WSACleanup();
		return 1;
	}

	g_hwnd = hwnd;

	// Start background frame fetch thread
	std::thread worker(WorkerThread);

	ShowWindow(hwnd, nCmdShow);
	UpdateWindow(hwnd);

	// Message loop
	MSG msg;
	while (GetMessageW(&msg, nullptr, 0, 0)) {
		TranslateMessage(&msg);
		DispatchMessageW(&msg);
	}

	// Shut down worker
	g_running.store(false);
	Disconnect(); // unblocks recv in worker
	if (worker.joinable()) worker.join();

	timeEndPeriod(1);
	WSACleanup();
	return static_cast<int>(msg.wParam);
}
