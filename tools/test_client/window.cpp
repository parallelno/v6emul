#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <WinSock2.h>
#include <WS2tcpip.h>
#include <Windows.h>

#include <cstring>
#include <format>

#include "keyboard.h"
#include "net.h"
#include "window.h"
#include "worker.h"

static constexpr UINT_PTR TIMER_ID = 1;
static constexpr UINT     TIMER_MS = 10;

HWND             g_hwnd     = nullptr;
BITMAPINFOHEADER g_bmiHeader{};

// Paint buffer: UI thread copies g_frameFront here under lock, then paints without lock
static std::vector<uint32_t> g_paintBuffer(MAX_FRAME_PIXELS, 0);

static int   g_fps         = 0;
static int   g_frameCount  = 0;
static DWORD g_lastFpsTick = 0;

LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
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
				g_fps         = g_recvCount.exchange(0);
				g_frameCount  = 0;
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

		size_t copyBytes = static_cast<size_t>(fw) * fh * 4;
		{
			std::lock_guard lock(g_frameMutex);
			memcpy(g_paintBuffer.data(), g_frameFront.data(), copyBytes);
		}

		g_bmiHeader.biWidth  = fw;
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
		if (mapped == VK_ESCAPE) DestroyWindow(hwnd);
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
