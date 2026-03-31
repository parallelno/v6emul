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
#pragma comment(lib, "ws2_32.lib")
#pragma comment(lib, "shell32.lib")

#include <cstdint>
#include <cstring>
#include <vector>
#include <string>
#include <format>

#include <nlohmann/json.hpp>
#include "ipc/protocol.h"
#include "ipc/commands.h"

// ── Frame constants (must match Display) ──────────────────────────────
static constexpr int FRAME_W = 768;
static constexpr int FRAME_H = 312;
static constexpr int FRAME_PIXELS = FRAME_W * FRAME_H;
static constexpr int FRAME_BYTES = FRAME_PIXELS * 4;

// Scale factor for the window (768×312 is quite wide and short)
static constexpr int SCALE = 2;

// Timer ID for the 50fps poll
static constexpr UINT_PTR TIMER_ID = 1;
static constexpr UINT TIMER_MS = 20; // 50 fps

// ── Globals ───────────────────────────────────────────────────────────
static SOCKET g_sock = INVALID_SOCKET;
static std::vector<uint32_t> g_frameBuf(FRAME_PIXELS, 0);
static bool g_connected = false;
static uint16_t g_port = 9876;
static BITMAPINFO g_bmi{};
static int g_fps = 0;
static int g_frameCount = 0;
static DWORD g_lastFpsTick = 0;

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

	// Disable Nagle
	BOOL nodelay = TRUE;
	setsockopt(g_sock, IPPROTO_TCP, TCP_NODELAY,
		reinterpret_cast<const char*>(&nodelay), sizeof(nodelay));

	sockaddr_in addr{};
	addr.sin_family = AF_INET;
	addr.sin_port = htons(g_port);
	inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);

	if (connect(g_sock, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == SOCKET_ERROR) {
		closesocket(g_sock);
		g_sock = INVALID_SOCKET;
		return false;
	}

	g_connected = true;
	return true;
}

static void Disconnect()
{
	if (g_sock != INVALID_SOCKET) {
		closesocket(g_sock);
		g_sock = INVALID_SOCKET;
	}
	g_connected = false;
}

// ── Protocol: send request, receive response ──────────────────────────
static bool SendRequest(const nlohmann::json& reqJ)
{
	auto encoded = dev::ipc::Encode(reqJ);
	return SendExact(encoded.data(), encoded.size());
}

static bool RecvResponse(nlohmann::json& outJ)
{
	uint32_t len = 0;
	if (!RecvExact(&len, 4)) return false;

	// Sanity check (64MB max)
	if (len > 64 * 1024 * 1024) return false;

	std::vector<uint8_t> payload(len);
	if (!RecvExact(payload.data(), len)) return false;

	outJ = dev::ipc::Decode(payload);
	return true;
}

// ── Fetch one frame from the emulator ─────────────────────────────────
static bool FetchFrame()
{
	if (!g_connected) {
		if (!ConnectToServer()) return false;
	}

	nlohmann::json reqJ = {
		{dev::ipc::FIELD_CMD, dev::ipc::CMD_GET_FRAME},
		{dev::ipc::FIELD_DATA, nullptr}
	};

	if (!SendRequest(reqJ)) {
		Disconnect();
		return false;
	}

	nlohmann::json respJ;
	if (!RecvResponse(respJ)) {
		Disconnect();
		return false;
	}

	if (!respJ.value(dev::ipc::FIELD_OK, false)) return false;

	auto& data = respJ[dev::ipc::FIELD_DATA];
	if (!data.contains("pixels")) return false;

	auto& pixBin = data["pixels"].get_binary();
	if (pixBin.size() != static_cast<size_t>(FRAME_BYTES)) return false;

	// Copy and convert ABGR (0xFFBBGGRR) → BGRA for DIB (0x00RRGGBB as uint32 LE)
	auto* src = reinterpret_cast<const uint32_t*>(pixBin.data());
	for (int i = 0; i < FRAME_PIXELS; ++i) {
		uint32_t px = src[i];
		uint32_t r = (px >>  0) & 0xFF;
		uint32_t g = (px >>  8) & 0xFF;
		uint32_t b = (px >> 16) & 0xFF;
		g_frameBuf[i] = (r << 16) | (g << 8) | b;
	}

	return true;
}

// ── Window procedure ──────────────────────────────────────────────────
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
			if (FetchFrame()) {
				g_frameCount++;
				InvalidateRect(hwnd, nullptr, FALSE);
			}
			// Update FPS counter every second
			DWORD now = GetTickCount();
			if (now - g_lastFpsTick >= 1000) {
				g_fps = g_frameCount;
				g_frameCount = 0;
				g_lastFpsTick = now;

				auto title = std::format(L"v6emul test client  |  {}x{}  |  {} fps  |  {}",
					FRAME_W, FRAME_H, g_fps,
					g_connected ? L"connected" : L"disconnected");
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

		// DIB rows are bottom-up, so we use negative height to flip
		SetStretchBltMode(hdc, COLORONCOLOR);
		StretchDIBits(hdc,
			0, 0, clientW, clientH,   // dest
			0, 0, FRAME_W, FRAME_H,   // src
			g_frameBuf.data(), &g_bmi,
			DIB_RGB_COLORS, SRCCOPY);

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

	// Set up DIB info (top-down via negative height)
	g_bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
	g_bmi.bmiHeader.biWidth = FRAME_W;
	g_bmi.bmiHeader.biHeight = -FRAME_H; // top-down
	g_bmi.bmiHeader.biPlanes = 1;
	g_bmi.bmiHeader.biBitCount = 32;
	g_bmi.bmiHeader.biCompression = BI_RGB;

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

	ShowWindow(hwnd, nCmdShow);
	UpdateWindow(hwnd);

	// Message loop
	MSG msg;
	while (GetMessageW(&msg, nullptr, 0, 0)) {
		TranslateMessage(&msg);
		DispatchMessageW(&msg);
	}

	WSACleanup();
	return static_cast<int>(msg.wParam);
}
