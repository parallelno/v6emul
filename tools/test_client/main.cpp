// Test client: connects to the emulator IPC server, fetches frames at 50fps,
// and displays them in a Win32 GDI window.
//
// Usage: test_client.exe [--port 9876] [--test-local]

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <WinSock2.h>
#include <WS2tcpip.h>
#include <Windows.h>
#include <shellapi.h>
#include <timeapi.h>
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "winmm.lib")

#include <thread>
#include <nlohmann/json.hpp>

#include "ipc/protocol.h"
#include "ipc/commands.h"
#include "core/hardware_consts.h"
#include "net.h"
#include "worker.h"
#include "window.h"

// ── Entry point ───────────────────────────────────────────────────────
int WINAPI wWinMain(HINSTANCE hInstance, HINSTANCE, LPWSTR, int nCmdShow)
{
	int argc;
	LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
	for (int i = 1; i < argc; ++i) {
		if (wcscmp(argv[i], L"--port") == 0 && i + 1 < argc) {
			g_port = static_cast<uint16_t>(_wtoi(argv[i + 1]));
			++i;
		} else if (wcscmp(argv[i], L"--test-local") == 0) {
			g_testLocal = true;
		}
	}
	LocalFree(argv);

	// Set system timer resolution to 1ms for accurate sleep_for timing
	timeBeginPeriod(1);

	WSADATA wsa;
	if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
		MessageBoxW(nullptr, L"WSAStartup failed", L"Error", MB_OK);
		return 1;
	}

	// Set up DIB info: BI_RGB for standard BGRA byte order (server converts)
	g_bmiHeader.biSize        = sizeof(BITMAPINFOHEADER);
	g_bmiHeader.biWidth       = MAX_FRAME_W;
	g_bmiHeader.biHeight      = -MAX_FRAME_H; // top-down
	g_bmiHeader.biPlanes      = 1;
	g_bmiHeader.biBitCount    = 32;
	g_bmiHeader.biCompression = BI_RGB;

	// Pre-encode IPC requests (reused every iteration)
	nlohmann::json reqJ = {
		{dev::ipc::FIELD_CMD, dev::ipc::CMD_GET_FRAME_RAW},
		{dev::ipc::FIELD_DATA, nullptr}
	};
	g_requestBytes = dev::ipc::Encode(reqJ);

	{
		nlohmann::json statsReqJ = {
			{dev::ipc::FIELD_CMD, static_cast<int>(Req::GET_HW_MAIN_STATS)},
			{dev::ipc::FIELD_DATA, nullptr}
		};
		g_statsRequestBytes = dev::ipc::Encode(statsReqJ);
	}

	// Register window class
	WNDCLASSEXW wc{};
	wc.cbSize        = sizeof(wc);
	wc.style         = CS_HREDRAW | CS_VREDRAW;
	wc.lpfnWndProc   = WndProc;
	wc.hInstance     = hInstance;
	wc.hCursor       = LoadCursorW(nullptr, MAKEINTRESOURCEW(32512));
	wc.hbrBackground = static_cast<HBRUSH>(GetStockObject(BLACK_BRUSH));
	wc.lpszClassName = L"V6TestClient";
	RegisterClassExW(&wc);

	RECT winRect = {0, 0, MAX_FRAME_W * SCALE, MAX_FRAME_H * SCALE};
	AdjustWindowRect(&winRect, WS_OVERLAPPEDWINDOW, FALSE);

	HWND hwnd = CreateWindowExW(0, L"V6TestClient",
		L"v6emul test client  |  connecting...",
		WS_OVERLAPPEDWINDOW,
		CW_USEDEFAULT, CW_USEDEFAULT,
		winRect.right - winRect.left, winRect.bottom - winRect.top,
		nullptr, nullptr, hInstance, nullptr);

	if (!hwnd) { WSACleanup(); return 1; }

	g_hwnd = hwnd;

	std::thread worker(WorkerThread);

	ShowWindow(hwnd, nCmdShow);
	UpdateWindow(hwnd);

	MSG msg;
	while (GetMessageW(&msg, nullptr, 0, 0)) {
		TranslateMessage(&msg);
		DispatchMessageW(&msg);
	}

	g_running.store(false);
	Disconnect(); // unblocks recv in worker
	if (worker.joinable()) worker.join();

	timeEndPeriod(1);
	WSACleanup();
	return static_cast<int>(msg.wParam);
}
