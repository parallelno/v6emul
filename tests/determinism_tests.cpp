#include <iostream>
#include <cstring>
#include <vector>
#include <memory>
#include <string>

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

struct RunResult {
    uint64_t cc;
    uint64_t frames;
    bool halted;
    uint16_t pc, sp, af, bc, de, hl;
    std::vector<std::pair<uint8_t, uint8_t>> portOutputs; // port, value pairs
};

static RunResult runRom(const std::vector<uint8_t>& rom, bool haltExit,
                        uint64_t maxFrames, uint64_t maxCycles)
{
    RunResult r{};

    auto hw = std::make_unique<dev::Hardware>("", "", true);

    hw->SetDebugPortOutCallback([&r](uint8_t port, uint8_t value) {
        r.portOutputs.push_back({port, value});
    });

    if (!rom.empty()) {
        nlohmann::json setMemJ = {{"addr", 0}, {"data", rom}};
        hw->Request(dev::Hardware::Req::SET_MEM, setMemJ);
    }

    hw->Request(dev::Hardware::Req::RESTART);

    nlohmann::json headlessJ = {
        {"haltExit", haltExit},
        {"maxFrames", maxFrames},
        {"maxCycles", maxCycles}
    };
    auto result = hw->Request(dev::Hardware::Req::RUN_HEADLESS, headlessJ);

    if (result) {
        auto resJ = *result;
        r.cc = resJ["cc"].get<uint64_t>();
        r.frames = resJ["frames"].get<uint64_t>();
        r.halted = resJ["halted"].get<bool>();
        r.pc = resJ["pc"].get<uint16_t>();
        r.sp = resJ["sp"].get<uint16_t>();
        r.af = resJ["af"].get<uint16_t>();
        r.bc = resJ["bc"].get<uint16_t>();
        r.de = resJ["de"].get<uint16_t>();
        r.hl = resJ["hl"].get<uint16_t>();
    }

    return r;
}

// Run the same ROM twice and verify deterministic results
static void test_determinism_halt() {
    // MVI A,0x42; OUT 0xED; MVI A,0x00; OUT 0xED; HLT
    std::vector<uint8_t> rom = { 0x3E, 0x42, 0xD3, 0xED, 0x3E, 0x00, 0xD3, 0xED, 0x76 };

    auto r1 = runRom(rom, true, 0, 0);
    auto r2 = runRom(rom, true, 0, 0);

    ASSERT_EQ(r1.cc, r2.cc);
    ASSERT_EQ(r1.frames, r2.frames);
    ASSERT_EQ(r1.halted, r2.halted);
    ASSERT_EQ(r1.pc, r2.pc);
    ASSERT_EQ(r1.sp, r2.sp);
    ASSERT_EQ(r1.af, r2.af);
    ASSERT_EQ(r1.bc, r2.bc);
    ASSERT_EQ(r1.de, r2.de);
    ASSERT_EQ(r1.hl, r2.hl);
    ASSERT_EQ(r1.portOutputs.size(), r2.portOutputs.size());
    for (size_t i = 0; i < r1.portOutputs.size(); i++) {
        ASSERT_EQ(r1.portOutputs[i].first, r2.portOutputs[i].first);
        ASSERT_EQ(r1.portOutputs[i].second, r2.portOutputs[i].second);
    }
}

// Run a more complex ROM (loop with arithmetic) for N frames and verify deterministic cycle count
static void test_determinism_frames() {
    // A program that loops doing arithmetic:
    // MVI A,0; MVI B,1; loop: ADD B; OUT 0xED; JMP loop
    std::vector<uint8_t> rom = {
        0x3E, 0x00,         // MVI A, 0
        0x06, 0x01,         // MVI B, 1
        // loop (addr 4):
        0x80,               // ADD B
        0xD3, 0xED,         // OUT 0xED
        0xC3, 0x04, 0x00    // JMP 0x0004
    };

    auto r1 = runRom(rom, false, 5, 0);
    auto r2 = runRom(rom, false, 5, 0);

    ASSERT_EQ(r1.cc, r2.cc);
    ASSERT_EQ(r1.frames, r2.frames);
    ASSERT_EQ(r1.pc, r2.pc);
    ASSERT_EQ(r1.af, r2.af);
    ASSERT_EQ(r1.portOutputs.size(), r2.portOutputs.size());
    for (size_t i = 0; i < r1.portOutputs.size(); i++) {
        ASSERT_EQ(r1.portOutputs[i].second, r2.portOutputs[i].second);
    }
}

// Run with cycle limit and verify determinism
static void test_determinism_cycles() {
    // Simple counting loop
    std::vector<uint8_t> rom = {
        0x3E, 0x00,         // MVI A, 0
        0x3C,               // INR A
        0xC3, 0x02, 0x00    // JMP 0x0002
    };

    auto r1 = runRom(rom, false, 0, 10000);
    auto r2 = runRom(rom, false, 0, 10000);

    ASSERT_EQ(r1.cc, r2.cc);
    ASSERT_EQ(r1.pc, r2.pc);
    ASSERT_EQ(r1.af, r2.af);
}

int main()
{
    test_determinism_halt();
    test_determinism_frames();
    test_determinism_cycles();

    std::cout << "Determinism Tests: " << tests_passed << "/" << tests_run << " passed";
    if (tests_failed > 0) {
        std::cout << " (" << tests_failed << " FAILED)";
    }
    std::cout << std::endl;
    return (tests_failed == 0) ? 0 : 1;
}
