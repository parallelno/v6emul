Implementation plan: --screenshot <path>

1. **Define the behavior**
Add this option to CLI help and documentation:

| Flag | Type | Default | Description |
| -----| -----| --------| ------------|
| --screenshot <path> | string | - | Store a screenshot in a file on exit |

Required behavior:

* Support .bmp and .jpg, with case-insensitive extension matching. format is
  selected by the path extension.
* Derive dimensions from the committed IO::State::displayMode, not reqDisplayMode:
  * MODE_256 (false): 256×256.
  * MODE_512 (true): 512×256.
* If the flag is present without a path, print an error to stdout, followed by
  the complete help text, then exit with code 1.
* If the flag is absent, preserve existing behavior.
* Screenshot is prionter on exit only.

Recommended scope, requiring confirmation before implementation:

* Capture once after headless execution stops through --run-frames, --run-cycles,
  or --halt-exit.
* Capture the active display area only, independently of the IPC options
  --frame-mode and --color-format.
* Reject screenshot usage without a headless stop condition, including server-
  only usage, rather than silently ignoring it.
* Capture the current rendered buffer at the stopping point; do not execute extra
  cycles to complete a frame. A stop mid-frame may therefore produce a partially
  updated image.

2. **Parse and validate the option**
Modify C:\Work\Programming\v6emul\app\main.cpp.

* Register screenshot as an optional string.
* Distinguish an absent flag from a present flag with an empty value. The existing
  GetString() returns the default in both cases.
* Reuse the parser’s existing mechanisms:
  * Detect presence using HasFlag("screenshot") without adding another help entry.
  * For an empty value, call RequirementMsg("screenshot") before IsRequirementSatisfied().
  * This already produces the required stdout message, then help, and supports returning code 1.
* Validate the extension and supported execution mode before constructing hardware.
* Reject unsupported or missing extensions with a clear diagnostic listing .bmp and .jpg.
* Cover both --screenshot at the end of the command and --screenshot --run-frames 10.

3. **Extract native-resolution pixels**

Add a screenshot capture helper, keeping image encoding outside the emulation core.

Relevant existing code:

* C:\Work\Programming\v6emul\libs\v6core\include\core\io.h
* C:\Work\Programming\v6emul\libs\v6core\include\core\display.h
* C:\Work\Programming\v6emul\libs\v6core\src\display.cpp

The internal framebuffer is always 768×312, and 256 mode duplicates horizontal pixels. Therefore:

1. Read the display mode and copy pixels while headless execution is stopped.
2. Extract the 512×256 active area using existing display offsets/constants.
3. In 512 mode, retain every pixel.
4. In 256 mode, retain one pixel from each horizontal pair; do not interpolate or resize vertically.
5. Convert the internal packed color values explicitly into RGB bytes, preserving orientation and channel order.
6. Reuse existing helper functions wherever practical.

Avoid changing shared display settings merely to obtain a screenshot.

4. **Encode and save the image**

Add proposed files:

* C:\Work\Programming\v6emul\app\screenshot.h
* C:\Work\Programming\v6emul\app\screenshot.cpp

Responsibilities:

* Select the encoder from the normalized extension.
* Write binary image data.
* Return useful errors for open, encoding, write, or close failures.
* Propagate failure as process exit code 1.

**Encoder decision:** the repository currently has no image-encoding dependency.
Select a portable BMP/JPEG solution before implementation; adding a dependency
requires an explicit build/dependency change. Avoid a Windows-only solution
because the project supports Linux and macOS.

Also define and document JPEG quality, overwrite behavior, and whether parent
directories must already exist.

5. **Integrate capture with execution**
Update:

C:\Work\Programming\v6emul\app\test_mode.h
C:\Work\Programming\v6emul\app\test_mode.cpp
C:\Work\Programming\v6emul\app\CMakeLists.txt

Pass the screenshot configuration into RunTestMode(). Save exactly once after
successful RUN_HEADLESS completion and before returning. Preserve existing test-
output formatting and avoid unsolicited success messages that could disrupt
golden tests.

6. **Add automated tests**
Register tests in C:\Work\Programming\v6emul\tests\CMakeLists.txt, following the
existing standalone executable and CTest patterns.

Test:

* Missing path: stdout error precedes help, exit 1, no file.
* Empty path and another option immediately after the flag.
* Unsupported/missing extension; uppercase .BMP and .JPG.
* Both formats in both display modes, checking actual encoded dimensions.
* Correct crop, horizontal sampling, orientation, and RGB channel order.
* Unwritable destination and missing parent directory.
* Capture after each supported stop condition.
* No screenshot requested: existing golden output remains unchanged.

Use exact pixel assertions for BMP; use decoded dimensions and tolerant color
comparisons for lossy JPEG.

7. **Document and validate**

Update C:\Work\Programming\v6emul\docs\cli.md, plus build documentation if a
dependency is introduced.

Build and run the full suite, including existing golden tests, then independently
open/decode representative BMP and JPEG files.

**Inspection completed:** reviewed CLI parsing, display handling, headless
execution, dependencies, and test configuration; ran the existing executable’s
--help successfully. No files were modified.

Suggested commit after implementation and validation:
feat(cli): add native-resolution BMP and JPG screenshots