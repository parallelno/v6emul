#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "core/display.h"

namespace dev
{
	enum class ScreenshotFormat : uint8_t
	{
		BMP = 0,
		JPG
	};

	struct ScreenshotFrame
	{
		int width = 0;
		int height = 0;
		std::vector<uint8_t> rgb; // width * height * 3 bytes, top-down RGB
	};

	// Detect the output format from the file extension. Matching is
	// case-insensitive. Returns false if the extension is missing or unsupported.
	auto DetectScreenshotFormat(const std::string& _path, ScreenshotFormat& _format) -> bool;

	// Extract the active display area from the internal 768x312 ABGR framebuffer.
	// The 512 mode yields 512x256; the 256 mode yields 256x256 by sampling every
	// other horizontally duplicated pixel.
	auto CaptureScreenshot(const Display::FrameBuffer& _frame, bool _displayMode512)
		-> ScreenshotFrame;

	// Capture and write a screenshot. Returns false and fills _error on failure.
	auto SaveScreenshot(const std::string& _path, const Display::FrameBuffer& _frame,
		bool _displayMode512, std::string& _error) -> bool;
}