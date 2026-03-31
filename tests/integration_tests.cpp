#include <iostream>
#include <cstring>
#include <vector>
#include <memory>

#include "core/hardware.h"
#include "utils/utils.h"

static int tests_run = 0;
static int tests_passed = 0;
static int tests_failed = 0;

#define ASSERT_EQ(a, b) do { \
    tests_run++; \
    if ((a) == (b)) { tests_passed++; } \
    else { \
        tests_failed++; \
        std::cerr << "FAIL: " << #a << " == " << #b \
                  << " (got " << (int)(a) << " != " << (int)(b) << ")" \
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

struct TestHW {
    std::unique_ptr<dev::Hardware> hw;
    std::vector<uint8_t> portOutputs;

    TestHW() : hw(std::make_unique<dev::Hardware>("", "", true)) {
        hw->SetDebugPortOutCallback([this](uint8_t port, uint8_t value) {
            portOutputs.push_back(value);
        });
    }

    void loadAndRun(const std::vector<uint8_t>& rom, bool haltExit,
                    uint64_t maxFrames = 0, uint64_t maxCycles = 0)
    {
        nlohmann::json setMemJ = {{"addr", 0}, {"data", rom}};
        hw->Request(dev::Hardware::Req::SET_MEM, setMemJ);
        hw->Request(dev::Hardware::Req::RESTART);

        nlohmann::json headlessJ = {
            {"haltExit", haltExit},
            {"maxFrames", maxFrames},
            {"maxCycles", maxCycles}
        };
        auto result = hw->Request(dev::Hardware::Req::RUN_HEADLESS, headlessJ);
        if (result) resJ = *result;
    }

    nlohmann::json resJ;
};

// Test: Simple HLT
static void test_hlt_basic() {
    TestHW t;
    t.loadAndRun({ 0x76 }, true); // HLT
    ASSERT_TRUE(t.resJ["halted"].get<bool>());
    ASSERT_EQ(t.resJ["pc"].get<uint16_t>(), 0x0000);
}

// Test: NOP + HLT
static void test_nop_hlt() {
    TestHW t;
    t.loadAndRun({ 0x00, 0x00, 0x00, 0x76 }, true); // NOP NOP NOP HLT
    ASSERT_TRUE(t.resJ["halted"].get<bool>());
    ASSERT_EQ(t.resJ["pc"].get<uint16_t>(), 0x0003);
}

// Test: Port 0xED output captures values correctly
static void test_port_output() {
    TestHW t;
    // MVI A,0x42; OUT 0xED; MVI A,0x00; OUT 0xED; HLT
    t.loadAndRun({ 0x3E, 0x42, 0xD3, 0xED, 0x3E, 0x00, 0xD3, 0xED, 0x76 }, true);
    ASSERT_TRUE(t.resJ["halted"].get<bool>());
    ASSERT_EQ(t.portOutputs.size(), (size_t)2);
    ASSERT_EQ(t.portOutputs[0], 0x42);
    ASSERT_EQ(t.portOutputs[1], 0x00);
}

// Test: Arithmetic produces correct register state
static void test_arithmetic() {
    TestHW t;
    // MVI A,10; MVI B,20; ADD B; HLT
    t.loadAndRun({ 0x3E, 0x0A, 0x06, 0x14, 0x80, 0x76 }, true);
    auto af = t.resJ["af"].get<uint16_t>();
    uint8_t a = af >> 8;
    ASSERT_EQ(a, 0x1E); // 10 + 20 = 30
}

// Test: Memory store and load via STA/LDA
static void test_memory_sta_lda() {
    TestHW t;
    // MVI A,0x77; STA 0x5000; MVI A,0x00; LDA 0x5000; HLT
    t.loadAndRun({
        0x3E, 0x77,
        0x32, 0x00, 0x50,
        0x3E, 0x00,
        0x3A, 0x00, 0x50,
        0x76
    }, true);
    auto af = t.resJ["af"].get<uint16_t>();
    uint8_t a = af >> 8;
    ASSERT_EQ(a, 0x77);
}

// Test: CALL/RET subroutine
static void test_call_ret() {
    TestHW t;
    // LXI SP,0x100; CALL 0x0010; OUT 0xED; HLT
    // (0x0010): MVI A,0x55; RET
    std::vector<uint8_t> rom(0x20, 0x00); // 32 bytes zeroed
    // Main code at 0x0000
    rom[0x00] = 0x31; rom[0x01] = 0x00; rom[0x02] = 0x01; // LXI SP, 0x100
    rom[0x03] = 0xCD; rom[0x04] = 0x10; rom[0x05] = 0x00; // CALL 0x0010
    rom[0x06] = 0xD3; rom[0x07] = 0xED;                    // OUT 0xED
    rom[0x08] = 0x76;                                       // HLT
    // Subroutine at 0x0010
    rom[0x10] = 0x3E; rom[0x11] = 0x55;                    // MVI A, 0x55
    rom[0x12] = 0xC9;                                       // RET

    t.loadAndRun(rom, true);
    ASSERT_TRUE(t.resJ["halted"].get<bool>());
    ASSERT_EQ(t.portOutputs.size(), (size_t)1);
    ASSERT_EQ(t.portOutputs[0], 0x55);
}

// Test: Loop with counter counts correctly
static void test_loop_counter() {
    TestHW t;
    // MVI B,5; MVI A,0
    // loop: ADD B; DCR B; JNZ loop; OUT 0xED; HLT
    // Expected: A = 5+4+3+2+1 = 15 = 0x0F
    t.loadAndRun({
        0x06, 0x05,         // MVI B, 5
        0x3E, 0x00,         // MVI A, 0
        // loop (addr 4):
        0x80,               // ADD B
        0x05,               // DCR B
        0xC2, 0x04, 0x00,   // JNZ 0x0004
        0xD3, 0xED,         // OUT 0xED
        0x76                // HLT
    }, true);
    ASSERT_EQ(t.portOutputs.size(), (size_t)1);
    ASSERT_EQ(t.portOutputs[0], 0x0F);
}

// Test: Run for N frames (cycle limit based)
static void test_run_frames() {
    TestHW t;
    // Infinite loop
    t.loadAndRun({ 0xC3, 0x00, 0x00 }, false, 3, 0); // JMP 0x0000, run 3 frames
    auto frames = t.resJ["frames"].get<uint64_t>();
    ASSERT_EQ(frames, (uint64_t)3);
}

// Test: Run for N cycles
static void test_run_cycles() {
    TestHW t;
    // NOP loop
    t.loadAndRun({ 0x00, 0xC3, 0x00, 0x00 }, false, 0, 100); // NOP; JMP 0
    auto cc = t.resJ["cc"].get<uint64_t>();
    ASSERT_TRUE(cc >= 100);
}

// Test: Stack push/pop preserves values
static void test_push_pop() {
    TestHW t;
    // LXI SP,0x100; LXI B,0xABCD; PUSH B; POP D; MOV A,D; OUT 0xED; MOV A,E; OUT 0xED; HLT
    t.loadAndRun({
        0x31, 0x00, 0x01,   // LXI SP, 0x100
        0x01, 0xCD, 0xAB,   // LXI B, 0xABCD
        0xC5,                // PUSH B
        0xD1,                // POP D
        0x7A,                // MOV A, D
        0xD3, 0xED,          // OUT 0xED
        0x7B,                // MOV A, E
        0xD3, 0xED,          // OUT 0xED
        0x76                 // HLT
    }, true);
    ASSERT_EQ(t.portOutputs.size(), (size_t)2);
    ASSERT_EQ(t.portOutputs[0], 0xAB); // D (high byte)
    ASSERT_EQ(t.portOutputs[1], 0xCD); // E (low byte)
}

int main()
{
    test_hlt_basic();
    test_nop_hlt();
    test_port_output();
    test_arithmetic();
    test_memory_sta_lda();
    test_call_ret();
    test_loop_counter();
    test_run_frames();
    test_run_cycles();
    test_push_pop();

    std::cout << "Integration Tests: " << tests_passed << "/" << tests_run << " passed";
    if (tests_failed > 0) {
        std::cout << " (" << tests_failed << " FAILED)";
    }
    std::cout << std::endl;
    return (tests_failed == 0) ? 0 : 1;
}
