# Test Client

The test client is a **Win32 GDI** application that connects to the v6emul IPC server, streams frames in real time, and displays them in a resizable window.

## Building

The test client is built automatically as part of the CMake project:

```bash
cmake --preset release
cmake --build --preset release
```

Output: `build/release/tools/test_client/Release/test_client.exe`

## Usage

```
test_client.exe [--port 9876]
```

Start the emulator first, then launch the test client. It auto-connects to `127.0.0.1` on the specified port (default 9876) and reconnects automatically if the connection drops.

Press **Escape** to close.

## How It Works

### Frame Streaming

The client runs a **worker thread** that continuously:

1. Sends `CMD_GET_FRAME_RAW` (`-4`) requests to the emulator
2. Receives raw binary responses: `[4:payloadLen][4:width][4:height][ABGR pixels]`
3. Writes pixels into a back buffer, then swaps to the front buffer under a mutex

The main (UI) thread runs a 15ms WM_TIMER that repaints whenever a new frame is ready, using `StretchDIBits` with BI_BITFIELDS for zero-copy ABGR rendering.

### Stats Polling

Every ~1 second, the worker thread sends a `CMD_GET_HW_MAIN_STATS` (43) request instead of a frame request. It extracts `speedPercent` from the MessagePack response and stores it in an atomic.

### Window Title

The title bar displays live information, updated once per second:

```
v6emul test client  |  768x312  |  50 fps  |  speed 100%  |  connected
```

| Field | Source |
|-------|--------|
| Resolution | Constant (768×312) |
| FPS | Counted from received frames per second |
| Speed | From `GET_HW_MAIN_STATS` `speedPercent` field |
| Status | Connection state (`connected` / `disconnected`) |

### Display Scaling

The frame (768×312) is scaled 2× by default to fill the window. The window is resizable — `StretchDIBits` handles arbitrary scaling with `COLORONCOLOR` mode.

### Connection Behavior

- Auto-connects on startup
- Reconnects automatically after disconnection (500ms retry interval)
- TCP_NODELAY enabled for low latency
- 4 MB receive buffer to absorb frame bursts
- Socket is non-blocking for the worker thread

## Architecture

```
┌─────────────────┐     TCP (loopback)     ┌──────────────┐
│   test_client    │ ◄──────────────────── │    v6emul     │
│                  │                        │              │
│  Worker Thread   │  GET_FRAME_RAW @ 50fps │  Emulation   │
│  (send/recv)     │  GET_HW_MAIN_STATS @1s │  Thread      │
│        │         │                        │              │
│        ▼         │                        └──────────────┘
│  Back Buffer     │
│   ↕ swap         │
│  Front Buffer    │
│        │         │
│        ▼         │
│  UI Thread       │
│  (WM_TIMER +     │
│   StretchDIBits) │
└─────────────────┘
```
