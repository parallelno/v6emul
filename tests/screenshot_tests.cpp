#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <string>
#include <vector>

#include "core/display.h"
#include "screenshot.h"

#define STB_IMAGE_IMPLEMENTATION
#include "stb_image.h"

static int tests_run = 0;
static int tests_passed = 0;
static int tests_failed = 0;

#define ASSERT_EQ(a, b) do { \
    tests_run++; \
    if ((a) == (b)) { tests_passed++; } \
    else { \
        tests_failed++; \
        std::cerr << "FAIL: " << #a << " == " << #b \
                  << " (got " << (a) << " != " << (b) << ")" \
                  << " at " << __FILE__ << ":" << __LINE__ << std::endl; \
    } \
} while(0)

#define ASSERT_TRUE(cond) do { \
    tests_run++; \
    if (cond) { tests_passed++; } \
    else { \
        tests_failed++; \
        std::cerr << "FAIL: " << #cond \
                  << " at " << __FILE__ << ":" << __LINE__ << std::endl; \
    } \
} while(0)

namespace
{
	auto TempPath(const std::string& _name) -> std::filesystem::path
	{
		auto dir = std::filesystem::temp_directory_path() / "v6emul_screenshot_tests";
		std::filesystem::create_directories(dir);
		return dir / _name;
	}

	uint32_t ReadLe32(const std::vector<uint8_t>& _data, size_t _offset)
	{
		return static_cast<uint32_t>(_data[_offset])
			| (static_cast<uint32_t>(_data[_offset + 1]) << 8)
			| (static_cast<uint32_t>(_data[_offset + 2]) << 16)
			| (static_cast<uint32_t>(_data[_offset + 3]) << 24);
	}

	uint16_t ReadLe16(const std::vector<uint8_t>& _data, size_t _offset)
	{
		return static_cast<uint16_t>(_data[_offset])
			| (static_cast<uint16_t>(_data[_offset + 1]) << 8);
	}

	auto ReadFile(const std::filesystem::path& _path) -> std::vector<uint8_t>
	{
		std::ifstream file(_path, std::ios::binary);
		return { std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>() };
	}

	auto MakeFrame() -> std::unique_ptr<dev::Display::FrameBuffer>
	{
		auto frame = std::make_unique<dev::Display::FrameBuffer>();
		frame->fill(0xff000000);
		return frame;
	}

	size_t FrameIndex(int _x, int _y)
	{
		return static_cast<size_t>(_y) * dev::Display::FRAME_W + _x;
	}

	void SetActivePixel(dev::Display::FrameBuffer& _frame, int _nativeX, int _nativeY,
		bool _displayMode512, uint32_t _color)
	{
		const int step = _displayMode512 ? 1 : 2;
		_frame[FrameIndex(dev::Display::BORDER_LEFT + _nativeX * step,
			dev::Display::SCAN_ACTIVE_AREA_TOP + _nativeY)] = _color;
	}

	bool Near(int _actual, int _expected, int _tolerance)
	{
		return _actual >= _expected - _tolerance && _actual <= _expected + _tolerance;
	}

	void TestDetectFormat()
	{
		dev::ScreenshotFormat format;
		ASSERT_TRUE(dev::DetectScreenshotFormat("shot.bmp", format));
		ASSERT_TRUE(format == dev::ScreenshotFormat::BMP);
		ASSERT_TRUE(dev::DetectScreenshotFormat("shot.BMP", format));
		ASSERT_TRUE(format == dev::ScreenshotFormat::BMP);
		ASSERT_TRUE(dev::DetectScreenshotFormat("shot.JpG", format));
		ASSERT_TRUE(format == dev::ScreenshotFormat::JPG);
		ASSERT_TRUE(!dev::DetectScreenshotFormat("shot.png", format));
		ASSERT_TRUE(!dev::DetectScreenshotFormat("shot", format));
	}

	void TestCapture512()
	{
		auto frame = MakeFrame();
		SetActivePixel(*frame, 0, 0, true, 0xff112233);        // ABGR -> R=0x33 G=0x22 B=0x11
		SetActivePixel(*frame, 511, 255, true, 0xffaabbcc);    // ABGR -> R=0xCC G=0xBB B=0xAA

		const auto shot = dev::CaptureScreenshot(*frame, true);
		ASSERT_EQ(shot.width, 512);
		ASSERT_EQ(shot.height, 256);
		ASSERT_EQ(shot.rgb.size(), static_cast<size_t>(512) * 256 * 3);
		ASSERT_EQ(shot.rgb[0], 0x33);
		ASSERT_EQ(shot.rgb[1], 0x22);
		ASSERT_EQ(shot.rgb[2], 0x11);
		ASSERT_EQ(shot.rgb[shot.rgb.size() - 3], 0xcc);
		ASSERT_EQ(shot.rgb[shot.rgb.size() - 2], 0xbb);
		ASSERT_EQ(shot.rgb[shot.rgb.size() - 1], 0xaa);
	}

	void TestCapture256()
	{
		auto frame = MakeFrame();
		SetActivePixel(*frame, 0, 0, false, 0xff0000ff);       // red
		SetActivePixel(*frame, 255, 255, false, 0xff00ff00);   // green

		// Odd pixels are not sampled and must not leak into the output.
		(*frame)[FrameIndex(dev::Display::BORDER_LEFT + 1,
			dev::Display::SCAN_ACTIVE_AREA_TOP)] = 0xffffffff;

		const auto shot = dev::CaptureScreenshot(*frame, false);
		ASSERT_EQ(shot.width, 256);
		ASSERT_EQ(shot.height, 256);
		ASSERT_EQ(shot.rgb[0], 0xff);
		ASSERT_EQ(shot.rgb[1], 0x00);
		ASSERT_EQ(shot.rgb[2], 0x00);
		ASSERT_EQ(shot.rgb[shot.rgb.size() - 3], 0x00);
		ASSERT_EQ(shot.rgb[shot.rgb.size() - 2], 0xff);
		ASSERT_EQ(shot.rgb[shot.rgb.size() - 1], 0x00);
	}

	void TestSaveBmp()
	{
		auto frame = MakeFrame();
		SetActivePixel(*frame, 0, 0, true, 0xff112233);
		SetActivePixel(*frame, 511, 255, true, 0xffaabbcc);

		const auto path = TempPath("capture.bmp");
		std::filesystem::remove(path);
		std::string error;
		ASSERT_TRUE(dev::SaveScreenshot(path.string(), *frame, true, error));
		ASSERT_TRUE(error.empty());

		const auto data = ReadFile(path);
		ASSERT_TRUE(data.size() > 54);
		ASSERT_EQ(data[0], 'B');
		ASSERT_EQ(data[1], 'M');
		ASSERT_EQ(ReadLe32(data, 18), 512u);
		ASSERT_EQ(ReadLe32(data, 22), 256u);
		ASSERT_EQ(ReadLe16(data, 28), 24u);

		const auto pixelOffset = ReadLe32(data, 10);
		const auto rowSize = static_cast<size_t>(512) * 3;
		const auto topRow = pixelOffset + (256 - 1) * rowSize;
		const auto bottomRow = pixelOffset;
		// BMP stores pixels as BGR. Pixel (0,0) is in the last file row; the
		// bottom-right pixel is at the end of the first file row.
		ASSERT_EQ(data[topRow], 0x11);
		ASSERT_EQ(data[topRow + 1], 0x22);
		ASSERT_EQ(data[topRow + 2], 0x33);
		ASSERT_EQ(data[bottomRow + rowSize - 3], 0xaa);
		ASSERT_EQ(data[bottomRow + rowSize - 2], 0xbb);
		ASSERT_EQ(data[bottomRow + rowSize - 1], 0xcc);

		std::filesystem::remove(path);
	}

	void TestSaveJpg()
	{
		auto frame = MakeFrame();
		// Use solid blocks: a single pixel would be destroyed by JPEG's lossy
		// chroma subsampling and would not test channel order meaningfully.
		for (int y = 0; y < 32; ++y) {
			for (int x = 0; x < 32; ++x) {
				SetActivePixel(*frame, x, y, false, 0xff0000ff);
				SetActivePixel(*frame, 255 - x, 255 - y, false, 0xff00ff00);
			}
		}

		const auto path = TempPath("capture.jpg");
		std::filesystem::remove(path);
		std::string error;
		ASSERT_TRUE(dev::SaveScreenshot(path.string(), *frame, false, error));
		ASSERT_TRUE(error.empty());

		int width = 0;
		int height = 0;
		int channels = 0;
		auto* pixels = stbi_load(path.string().c_str(), &width, &height, &channels, 3);
		ASSERT_TRUE(pixels != nullptr);
		ASSERT_EQ(width, 256);
		ASSERT_EQ(height, 256);
		if (pixels) {
			const auto topLeft = (static_cast<size_t>(16) * width + 16) * 3;
			ASSERT_TRUE(Near(pixels[topLeft], 255, 40));
			ASSERT_TRUE(Near(pixels[topLeft + 1], 0, 40));
			ASSERT_TRUE(Near(pixels[topLeft + 2], 0, 40));
			const auto bottomRight = (static_cast<size_t>(240) * width + 240) * 3;
			ASSERT_TRUE(Near(pixels[bottomRight], 0, 40));
			ASSERT_TRUE(Near(pixels[bottomRight + 1], 255, 40));
			ASSERT_TRUE(Near(pixels[bottomRight + 2], 0, 40));
			stbi_image_free(pixels);
		}

		std::filesystem::remove(path);
	}

	void TestSaveErrors()
	{
		auto frame = MakeFrame();
		std::string error;
		ASSERT_TRUE(!dev::SaveScreenshot("capture.png", *frame, true, error));
		ASSERT_TRUE(!error.empty());

		error.clear();
		const auto missingDir = TempPath("missing") / "capture.bmp";
		std::filesystem::remove_all(missingDir.parent_path());
		ASSERT_TRUE(!dev::SaveScreenshot(missingDir.string(), *frame, true, error));
		ASSERT_TRUE(!error.empty());
	}
}

int main()
{
	TestDetectFormat();
	TestCapture512();
	TestCapture256();
	TestSaveBmp();
	TestSaveJpg();
	TestSaveErrors();

	std::cout << "screenshot_tests: " << tests_passed << "/" << tests_run << " passed";
	if (tests_failed > 0) {
		std::cout << ", " << tests_failed << " failed";
	}
	std::cout << std::endl;

	return tests_failed == 0 ? 0 : 1;
}