#include "screenshot.h"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <format>

#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb_image_write.h"

namespace
{
	constexpr int JPEG_QUALITY = 90;

	auto ToLower(std::string _value) -> std::string
	{
		std::transform(_value.begin(), _value.end(), _value.begin(),
			[](unsigned char _ch) { return static_cast<char>(std::tolower(_ch)); });
		return _value;
	}
}

auto dev::DetectScreenshotFormat(const std::string& _path, ScreenshotFormat& _format) -> bool
{
	const auto extension = ToLower(std::filesystem::path(_path).extension().string());
	if (extension == ".bmp") {
		_format = ScreenshotFormat::BMP;
		return true;
	}
	if (extension == ".jpg") {
		_format = ScreenshotFormat::JPG;
		return true;
	}
	return false;
}

auto dev::CaptureScreenshot(const Display::FrameBuffer& _frame, bool _displayMode512)
	-> ScreenshotFrame
{
	ScreenshotFrame result;
	result.width = _displayMode512 ? Display::ACTIVE_AREA_W : Display::ACTIVE_AREA_W / 2;
	result.height = Display::ACTIVE_AREA_H;
	result.rgb.resize(static_cast<size_t>(result.width) * result.height * 3);

	const int pixelStep = _displayMode512 ? 1 : 2;
	const int sourceX = Display::BORDER_LEFT;
	const int sourceY = Display::SCAN_ACTIVE_AREA_TOP;

	size_t dst = 0;
	for (int y = 0; y < result.height; ++y)
	{
		const size_t row = static_cast<size_t>(sourceY + y) * Display::FRAME_W;
		for (int x = 0; x < result.width; ++x)
		{
			// The internal color is packed ABGR: 0xAABBGGRR.
			const auto color = _frame[row + sourceX + static_cast<size_t>(x) * pixelStep];
			result.rgb[dst++] = static_cast<uint8_t>(color);
			result.rgb[dst++] = static_cast<uint8_t>(color >> 8);
			result.rgb[dst++] = static_cast<uint8_t>(color >> 16);
		}
	}

	return result;
}

auto dev::SaveScreenshot(const std::string& _path, const Display::FrameBuffer& _frame,
	bool _displayMode512, std::string& _error) -> bool
{
	ScreenshotFormat format;
	if (!DetectScreenshotFormat(_path, format)) {
		_error = std::format(
			"unsupported screenshot extension for '{}' (supported: .bmp, .jpg)", _path);
		return false;
	}

	const auto parent = std::filesystem::path(_path).parent_path();
	if (!parent.empty() && !std::filesystem::is_directory(parent)) {
		_error = std::format("screenshot directory does not exist: {}", parent.string());
		return false;
	}

	const auto image = CaptureScreenshot(_frame, _displayMode512);
	int written = 0;
	if (format == ScreenshotFormat::BMP) {
		written = stbi_write_bmp(_path.c_str(), image.width, image.height, 3, image.rgb.data());
	}
	else {
		written = stbi_write_jpg(
			_path.c_str(), image.width, image.height, 3, image.rgb.data(), JPEG_QUALITY);
	}

	if (written == 0) {
		_error = std::format("failed to write screenshot: {}", _path);
		return false;
	}

	return true;
}