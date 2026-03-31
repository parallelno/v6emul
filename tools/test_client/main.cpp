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

// ── DIB with BI_BITFIELDS for direct ABGR rendering ──────────────────
struct BitmapInfoBF {
	BITMAPINFOHEADER bmiHeader;
	DWORD masks[3]; // R, G, B channel masks
};

// ── Frame constants (must match Display) ──────────────────────────────
static constexpr int FRAME_W = 768;
static constexpr int FRAME_H = 312;
static constexpr int FRAME_PIXELS = FRAME_W * FRAME_H;
static constexpr int FRAME_BYTES = FRAME_PIXELS * 4;
// Scale factor for the window (768×312 is quite wide and short)
static constexpr int SCALE = 2;

// Timer ID for repaint
static constexpr UINT_PTR TIMER_ID = 1;
static constexpr UINT TIMER_MS = 15;

// ── Globals ───────────────────────────────────────────────────────────
static SOCKET g_sock = INVALID_SOCKET;
static uint16_t g_port = 9876;
static BitmapInfoBF g_bmi{};
static std::vector<uint8_t> g_requestBytes;
static std::vector<uint8_t> g_statsRequestBytes;

// Double-buffered frames: worker writes to back, UI reads from front
static std::vector<uint32_t> g_frameFront(FRAME_PIXELS, 0);
static std::vector<uint32_t> g_frameBack(FRAME_PIXELS, 0);
static std::mutex g_frameMutex;
static std::atomic<bool> g_frameReady{false};

// Stats
static std::atomic<int> g_recvCount{0};
static std::atomic<bool> g_connected{false};
static std::atomic<int> g_speedPercent{0};

// Worker thread control
static std::atomic<bool> g_running{true};
static HWND g_hwnd = nullptr;

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

// ── Worker thread: fetch frames at 50 fps ─────────────────────────────
static void WorkerThread()
{
	using clock = std::chrono::system_clock;
	static constexpr auto FRAME_INTERVAL = std::chrono::microseconds(20000); // 50 fps
	static constexpr auto SLEEP_SLICE = std::chrono::microseconds(100);

	DWORD lastStatsTick = GetTickCount();
	auto endFrameTime = clock::now();

	while (g_running.load()) {
		if (!g_connected.load()) {
			if (!ConnectToServer()) {
				Sleep(500);
				endFrameTime = clock::now();
				continue;
			}
		}

		auto startFrameTime = clock::now();

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
		if (width != FRAME_W || height != FRAME_H || pixelBytes != FRAME_BYTES) {
			Disconnect();
			continue;
		}

		// Receive directly into back buffer
		if (!RecvExact(g_frameBack.data(), pixelBytes)) { Disconnect(); continue; }

		// Swap to front under lock
		{
			std::lock_guard lock(g_frameMutex);
			g_frameFront.swap(g_frameBack);
		}
		g_frameReady.store(true);
		g_recvCount.fetch_add(1);

		// vsync: same approach as hardware.cpp
		auto frameDuration = clock::now() - startFrameTime;
		using DurationType = decltype(frameDuration);
		endFrameTime += std::max<DurationType>(frameDuration, FRAME_INTERVAL);

		while (clock::now() < endFrameTime)
		{
			std::this_thread::sleep_for(SLEEP_SLICE);
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
		timeBeginPeriod(1);
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
					FRAME_W, FRAME_H, g_fps, g_speedPercent.load(),
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

		{
			std::lock_guard lock(g_frameMutex);
			SetStretchBltMode(hdc, COLORONCOLOR);
			StretchDIBits(hdc,
				0, 0, clientW, clientH,
				0, 0, FRAME_W, FRAME_H,
				g_frameFront.data(), reinterpret_cast<const BITMAPINFO*>(&g_bmi),
				DIB_RGB_COLORS, SRCCOPY);
		}

		EndPaint(hwnd, &ps);
		return 0;
	}

	case WM_KEYDOWN:
		if (wParam == VK_ESCAPE) {
			DestroyWindow(hwnd);
		}
		return 0;

	case WM_DESTROY:
		KillTimer(hwnd, TIMER_ID);
		timeEndPeriod(1);
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
	for (int i = 1; i < argc - 1; ++i) {
		if (wcscmp(argv[i], L"--port") == 0) {
			g_port = static_cast<uint16_t>(_wtoi(argv[i + 1]));
		}
	}
	LocalFree(argv);

	// Init Winsock
	WSADATA wsa;
	if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
		MessageBoxW(nullptr, L"WSAStartup failed", L"Error", MB_OK);
		return 1;
	}

	// Set up DIB info: BI_BITFIELDS for direct ABGR rendering (no per-pixel conversion)
	g_bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
	g_bmi.bmiHeader.biWidth = FRAME_W;
	g_bmi.bmiHeader.biHeight = -FRAME_H; // top-down
	g_bmi.bmiHeader.biPlanes = 1;
	g_bmi.bmiHeader.biBitCount = 32;
	g_bmi.bmiHeader.biCompression = BI_BITFIELDS;
	g_bmi.masks[0] = 0x000000FF; // R in low byte
	g_bmi.masks[1] = 0x0000FF00; // G
	g_bmi.masks[2] = 0x00FF0000; // B

	// Pre-encode the frame request (reused every frame)
	nlohmann::json reqJ = {
		{dev::ipc::FIELD_CMD, dev::ipc::CMD_GET_FRAME_RAW},
		{dev::ipc::FIELD_DATA, nullptr}
	};
	g_requestBytes = dev::ipc::Encode(reqJ);

	// Pre-encode the stats request
	{
		nlohmann::json statsReqJ = {
			{dev::ipc::FIELD_CMD, dev::ipc::CMD_GET_HW_MAIN_STATS},
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
	RECT winRect = {0, 0, FRAME_W * SCALE, FRAME_H * SCALE};
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

	WSACleanup();
	return static_cast<int>(msg.wParam);
}
