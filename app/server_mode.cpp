#include "server_mode.h"

#include <iostream>
#include <string>
#include <format>
#include <vector>
#include <cstdint>
#include <cstring>
#include <atomic>
#include <csignal>

#include "core/hardware.h"
#include "ipc/transport.h"
#include "ipc/protocol.h"
#include "ipc/commands.h"

static std::atomic<bool> g_shutdown{false};
static dev::ipc::Transport* g_serverPtr = nullptr;

static void SignalHandler(int)
{
	g_shutdown.store(true);
	// Close the server sockets to unblock accept()/recv()
	if (g_serverPtr) g_serverPtr->Close();
}

int RunServerMode(dev::Hardware& _hw, uint16_t _port, dev::Display::ColorFormat _colorFormat)
{
	std::signal(SIGINT, SignalHandler);
	std::signal(SIGTERM, SignalHandler);

	dev::ipc::Transport server;
	g_serverPtr = &server;

	if (!server.Listen(_port)) {
		std::cerr << std::format("Error: failed to listen on port {}", _port) << std::endl;
		return 1;
	}

	std::cout << std::format("IPC server listening on 127.0.0.1:{} (Ctrl+C to stop)", server.GetPort()) << std::endl;

	// Start emulation in RUN state
	_hw.Request(dev::Hardware::Req::RUN);
    _hw.Request(dev::Hardware::Req::SET_COLOR_FORMAT,
		{{"colorFormat", static_cast<int>(_colorFormat)}});

	while (!g_shutdown.load()) {
		if (!server.IsClientConnected()) {
			std::cout << "Waiting for client..." << std::endl;
			if (!server.AcceptClient()) {
				if (g_shutdown.load()) break;
				std::cerr << "Error: failed to accept client" << std::endl;
				continue;
			}
			std::cout << "Client connected" << std::endl;
		}

		// Receive one message
		auto payload = server.Recv();
		if (payload.empty()) {
			if (g_shutdown.load()) break;
			std::cout << "Client disconnected" << std::endl;
			server.Close();
			// Re-listen for next client
			if (!server.Listen(_port)) {
				std::cerr << "Error: failed to re-listen" << std::endl;
				return 1;
			}
			continue;
		}

		// Decode request
		nlohmann::json requestJ;
		try {
			requestJ = dev::ipc::Decode(payload);
		} catch (const std::exception& e) {
			auto errResp = dev::ipc::Encode(
				dev::ipc::MakeErrorResponse(std::format("decode error: {}", e.what())));
			server.Send(errResp);
			continue;
		}

		int cmdInt = requestJ.value(dev::ipc::FIELD_CMD, 0);
		auto dataJ = requestJ.value(dev::ipc::FIELD_DATA, nlohmann::json{});

		// Handle pseudo-commands
		if (cmdInt == dev::ipc::CMD_PING) {
			auto resp = dev::ipc::Encode(dev::ipc::MakeResponse({{"pong", true}}));
			server.Send(resp);
			continue;
		}

		if (cmdInt == dev::ipc::CMD_GET_FRAME) {
			auto [pixels, region] = _hw.GetFrame(true);
			nlohmann::json responseJ;
			if (pixels) {
				size_t pixLen = static_cast<size_t>(region.width) * region.height * sizeof(dev::ColorI);
				const auto* raw = reinterpret_cast<const uint8_t*>(pixels);
				responseJ = dev::ipc::MakeResponse({
					{"width", region.width},
					{"height", region.height},
					{"pixels", nlohmann::json::binary_t({raw, raw + pixLen})}
				});
			} else {
				responseJ = dev::ipc::MakeErrorResponse("no frame available");
			}
			auto respBytes = dev::ipc::Encode(responseJ);
			server.Send(respBytes);
			continue;
		}

		// Raw binary frame: [4:payloadLen][4:width][4:height][raw ABGR pixels]
		// Bypasses json/msgpack for high-throughput frame streaming.
		if (cmdInt == dev::ipc::CMD_GET_FRAME_RAW) {
			auto [pixels, region] = _hw.GetFrame(false);
			if (pixels) {
				size_t pixLen = region.GetByteLen();
				uint32_t payloadLen = static_cast<uint32_t>(8 + pixLen);

				// Reuse a static buffer to avoid per-frame allocation
				static std::vector<uint8_t> rawMsg;
				rawMsg.resize(4 + 8 + pixLen);
				std::memcpy(rawMsg.data(), &payloadLen, 4);
				std::memcpy(rawMsg.data() + 4, &region.width, 4);
				std::memcpy(rawMsg.data() + 8, &region.height, 4);
				std::memcpy(rawMsg.data() + 12, pixels, pixLen);

				server.Send(rawMsg);
			} else {
				auto errResp = dev::ipc::Encode(
					dev::ipc::MakeErrorResponse("no frame available"));
				server.Send(errResp);
			}
			continue;
		}

		// Dispatch to Hardware::Request()
		auto req = static_cast<dev::Hardware::Req>(cmdInt);

		// EXIT command: respond, then shut down
		if (req == dev::Hardware::Req::EXIT) {
			auto resp = dev::ipc::Encode(dev::ipc::MakeResponse({{"exiting", true}}));
			server.Send(resp);
			_hw.Request(dev::Hardware::Req::EXIT);
			break;
		}

		decltype(_hw.Request(req, dataJ)) result;
		try {
			result = _hw.Request(req, dataJ);
		} catch (const std::exception& e) {
			auto errResp = dev::ipc::Encode(
				dev::ipc::MakeErrorResponse(std::format("dispatch error: {}", e.what())));
			server.Send(errResp);
			continue;
		}

		nlohmann::json responseJ;
		if (result) {
			responseJ = dev::ipc::MakeResponse(*result);
		} else {
			responseJ = dev::ipc::MakeErrorResponse("request failed");
		}

		auto respBytes = dev::ipc::Encode(responseJ);
		if (!server.Send(respBytes)) {
			std::cout << "Client disconnected during send" << std::endl;
			server.Close();
			if (!server.Listen(_port)) {
				std::cerr << "Error: failed to re-listen" << std::endl;
				return 1;
			}
		}
	}

	server.Close();
	g_serverPtr = nullptr;
	if (g_shutdown.load()) {
		std::cout << "\nShutting down..." << std::endl;
		_hw.Request(dev::Hardware::Req::EXIT);
	}
	return 0;
}
