#include <iostream>
#include <cassert>
#include <cstring>
#include <vector>
#include <memory>
#include "core/memory.h"

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

// Helper: create a Memory with no boot/ramdisk files (heap-allocated to avoid stack overflow)
static std::unique_ptr<dev::Memory> MakeMemory() {
    return std::make_unique<dev::Memory>("", "", true);
}

static void test_init_zeros() {
    auto mem = MakeMemory();
    mem->Init();

    // After init, main RAM should be zero
    for (int i = 0; i < 256; i++) {
        ASSERT_EQ(mem->GetByte(static_cast<dev::Addr>(i)), 0);
    }
}

static void test_set_and_get_byte() {
    auto mem = MakeMemory();
    mem->Init();

    // Write a single byte and read it back
    std::vector<uint8_t> data = { 0x42 };
    mem->SetRam(0x1000, data);
    ASSERT_EQ(mem->GetByte(0x1000), 0x42);
}

static void test_set_ram_block() {
    auto mem = MakeMemory();
    mem->Init();

    // Write a block of bytes
    std::vector<uint8_t> data = { 0x01, 0x02, 0x03, 0x04, 0x05 };
    mem->SetRam(0x2000, data);

    ASSERT_EQ(mem->GetByte(0x2000), 0x01);
    ASSERT_EQ(mem->GetByte(0x2001), 0x02);
    ASSERT_EQ(mem->GetByte(0x2002), 0x03);
    ASSERT_EQ(mem->GetByte(0x2003), 0x04);
    ASSERT_EQ(mem->GetByte(0x2004), 0x05);
}

static void test_set_byte_global() {
    auto mem = MakeMemory();
    mem->Init();

    mem->SetByteGlobal(0x3000, 0xAB);
    ASSERT_EQ(mem->GetByteGlobal(0x3000), 0xAB);
    ASSERT_EQ(mem->GetByte(0x3000), 0xAB);
}

static void test_global_addr_main_ram() {
    auto mem = MakeMemory();
    mem->Init();

    // In main RAM with no RAM disk mapping, global addr == local addr
    auto globalAddr = mem->GetGlobalAddr(0x1234, dev::Memory::AddrSpace::RAM);
    ASSERT_EQ(globalAddr, 0x1234u);
}

static void test_rom_enabled_after_init() {
    auto mem = MakeMemory();
    mem->Init();

    // After Init(), ROM should be enabled
    ASSERT_TRUE(mem->IsRomEnabled());
}

static void test_rom_disabled_after_restart() {
    auto mem = MakeMemory();
    mem->Init();
    mem->Restart();

    // After Restart(), ROM should be disabled
    ASSERT_TRUE(!mem->IsRomEnabled());
}

static void test_boundary_addresses() {
    auto mem = MakeMemory();
    mem->Init();

    // Test first and last byte of main RAM
    mem->SetByteGlobal(0x0000, 0x11);
    mem->SetByteGlobal(0xFFFF, 0x22);
    ASSERT_EQ(mem->GetByteGlobal(0x0000), 0x11);
    ASSERT_EQ(mem->GetByteGlobal(0xFFFF), 0x22);
}

static void test_ramdisk_mapping_init() {
    auto mem = MakeMemory();
    mem->Init();

    // After init, mappings should be inactive
    auto mappings = mem->GetMappingsP();
    for (int i = 0; i < 8; i++) {
        ASSERT_EQ(mappings[i].data, 0);
    }
}

static void test_set_ramdisk_mode() {
    auto mem = MakeMemory();
    mem->Init();

    // Enable a RAM disk mode: stack mode on page 1
    // Bits: E=0, 8=0, A=0, S=1, ss=01, MM=00 = 0b00010100 = 0x14
    mem->SetRamDiskMode(0, 0x14);

    auto mappings = mem->GetMappingsP();
    ASSERT_TRUE(mappings[0].modeStack);
    ASSERT_EQ(mappings[0].pageStack, 1);
}

int main()
{
    test_init_zeros();
    test_set_and_get_byte();
    test_set_ram_block();
    test_set_byte_global();
    test_global_addr_main_ram();
    test_rom_enabled_after_init();
    test_rom_disabled_after_restart();
    test_boundary_addresses();
    test_ramdisk_mapping_init();
    test_set_ramdisk_mode();

    std::cout << "Memory Tests: " << tests_passed << "/" << tests_run << " passed";
    if (tests_failed > 0) {
        std::cout << " (" << tests_failed << " FAILED)";
    }
    std::cout << std::endl;
    return (tests_failed == 0) ? 0 : 1;
}
