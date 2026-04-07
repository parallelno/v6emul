#pragma once
#include <atomic>
#include <chrono>
#include <cstdint>
#include <mutex>
#include <vector>

// ── Frame constants ───────────────────────────────────────────────────
inline constexpr int  MAX_FRAME_W      = 768;
inline constexpr int  MAX_FRAME_H      = 312;
inline constexpr int  MAX_FRAME_PIXELS = MAX_FRAME_W * MAX_FRAME_H;
inline constexpr int  MAX_FRAME_BYTES  = MAX_FRAME_PIXELS * 4;
inline constexpr int  SCALE            = 1;
inline constexpr auto FRAME_INTERVAL   = std::chrono::microseconds(20'000);

// ── Key event ────────────────────────────────────────────────────────
struct KeyEvent { int keyCode; int action; };

// ── Shared state (worker produces, window consumes) ──────────────────
extern std::vector<uint32_t> g_frameFront;
extern std::vector<uint32_t> g_frameBack;
extern std::mutex            g_frameMutex;
extern std::atomic<bool>     g_frameReady;
extern std::atomic<int>      g_frameW;
extern std::atomic<int>      g_frameH;

// ── Worker control ────────────────────────────────────────────────────
extern std::atomic<bool>     g_running;
extern bool                  g_testLocal;

// ── Stats (worker writes, window reads) ──────────────────────────────
extern std::atomic<int>      g_recvCount;
extern std::atomic<int>      g_speedPercent;

// ── Pre-encoded IPC requests (set before worker starts) ──────────────
extern std::vector<uint8_t>  g_requestBytes;
extern std::vector<uint8_t>  g_statsRequestBytes;

// ── Key event queue (window pushes, worker drains) ───────────────────
extern std::vector<KeyEvent> g_keyQueue;
extern std::mutex            g_keyMutex;

void WorkerThread();
