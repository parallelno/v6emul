#include <iostream>
#include <cassert>
#include <cstring>
#include <vector>
#include <functional>
#include <memory>
#include "core/cpu_i8080.h"
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

// IO port storage for tests
static uint8_t io_ports_out[256] = {};
static uint8_t io_ports_in[256] = {};

static uint8_t testInput(uint8_t port) { return io_ports_in[port]; }
static void testOutput(uint8_t port, uint8_t value) { io_ports_out[port] = value; }

struct TestCPU {
    std::unique_ptr<dev::Memory> mem;
    std::unique_ptr<dev::CpuI8080> cpu;

    TestCPU()
        : mem(std::make_unique<dev::Memory>("", "", true))
    {
        mem->Init();
        mem->Restart(); // disable ROM so we use RAM
        cpu = std::make_unique<dev::CpuI8080>(*mem, testInput, testOutput);
        memset(io_ports_out, 0, sizeof(io_ports_out));
        memset(io_ports_in, 0, sizeof(io_ports_in));
    }

    void loadCode(dev::Addr addr, const std::vector<uint8_t>& code) {
        mem->SetRam(addr, code);
    }

    void runUntilHalt(int maxCycles = 10000) {
        for (int i = 0; i < maxCycles; i++) {
            cpu->ExecuteMachineCycle(false);
            if (cpu->GetHLTA()) break;
        }
    }

    void execOne() {
        do {
            cpu->ExecuteMachineCycle(false);
        } while (!cpu->IsInstructionExecuted());
    }
};

// ─── NOP ─────────────────────────────────────────────────────────────

static void test_nop() {
    TestCPU t;
    t.loadCode(0, { 0x00, 0x76 }); // NOP, HLT
    t.execOne(); // execute NOP
    ASSERT_EQ(t.cpu->GetPC(), 1);
}

// ─── MVI (Move Immediate) ───────────────────────────────────────────

static void test_mvi_b() {
    TestCPU t;
    // MVI B, 0x42 ; HLT
    t.loadCode(0, { 0x06, 0x42, 0x76 });
    t.execOne();
    ASSERT_EQ(t.cpu->GetB(), 0x42);
    ASSERT_EQ(t.cpu->GetPC(), 2);
}

static void test_mvi_a() {
    TestCPU t;
    // MVI A, 0xFF ; HLT
    t.loadCode(0, { 0x3E, 0xFF, 0x76 });
    t.execOne();
    ASSERT_EQ(t.cpu->GetA(), 0xFF);
}

static void test_mvi_all_regs() {
    TestCPU t;
    // MVI B,1 ; MVI C,2 ; MVI D,3 ; MVI E,4 ; MVI H,5 ; MVI L,6 ; MVI A,7 ; HLT
    t.loadCode(0, {
        0x06, 0x01,  // MVI B, 1
        0x0E, 0x02,  // MVI C, 2
        0x16, 0x03,  // MVI D, 3
        0x1E, 0x04,  // MVI E, 4
        0x26, 0x05,  // MVI H, 5
        0x2E, 0x06,  // MVI L, 6
        0x3E, 0x07,  // MVI A, 7
        0x76         // HLT
    });
    t.runUntilHalt();
    ASSERT_EQ(t.cpu->GetB(), 1);
    ASSERT_EQ(t.cpu->GetC(), 2);
    ASSERT_EQ(t.cpu->GetD(), 3);
    ASSERT_EQ(t.cpu->GetE(), 4);
    ASSERT_EQ(t.cpu->GetH(), 5);
    ASSERT_EQ(t.cpu->GetL(), 6);
    ASSERT_EQ(t.cpu->GetA(), 7);
}

// ─── MOV (Register-to-Register) ─────────────────────────────────────

static void test_mov_b_a() {
    TestCPU t;
    // MVI A, 0xAB ; MOV B, A ; HLT
    t.loadCode(0, { 0x3E, 0xAB, 0x47, 0x76 });
    t.runUntilHalt();
    ASSERT_EQ(t.cpu->GetB(), 0xAB);
}

// ─── LXI (Load Register Pair Immediate) ─────────────────────────────

static void test_lxi_bc() {
    TestCPU t;
    // LXI B, 0x1234 ; HLT
    t.loadCode(0, { 0x01, 0x34, 0x12, 0x76 });
    t.runUntilHalt();
    ASSERT_EQ(t.cpu->GetBC(), 0x1234);
}

static void test_lxi_sp() {
    TestCPU t;
    // LXI SP, 0xFF00 ; HLT
    t.loadCode(0, { 0x31, 0x00, 0xFF, 0x76 });
    t.runUntilHalt();
    ASSERT_EQ(t.cpu->GetSP(), 0xFF00);
}

// ─── ADD / ADI (Arithmetic) ─────────────────────────────────────────

static void test_add_b() {
    TestCPU t;
    // MVI A, 0x10 ; MVI B, 0x20 ; ADD B ; HLT
    t.loadCode(0, { 0x3E, 0x10, 0x06, 0x20, 0x80, 0x76 });
    t.runUntilHalt();
    ASSERT_EQ(t.cpu->GetA(), 0x30);
    ASSERT_TRUE(!t.cpu->GetFlagC());
    ASSERT_TRUE(!t.cpu->GetFlagZ());
}

static void test_add_carry() {
    TestCPU t;
    // MVI A, 0xFF ; MVI B, 0x01 ; ADD B ; HLT
    t.loadCode(0, { 0x3E, 0xFF, 0x06, 0x01, 0x80, 0x76 });
    t.runUntilHalt();
    ASSERT_EQ(t.cpu->GetA(), 0x00);
    ASSERT_TRUE(t.cpu->GetFlagC());
    ASSERT_TRUE(t.cpu->GetFlagZ());
}

static void test_adi() {
    TestCPU t;
    // MVI A, 0x14 ; ADI 0x42 ; HLT
    t.loadCode(0, { 0x3E, 0x14, 0xC6, 0x42, 0x76 });
    t.runUntilHalt();
    ASSERT_EQ(t.cpu->GetA(), 0x56);
}

// ─── SUB / SBI (Subtraction) ────────────────────────────────────────

static void test_sub_b() {
    TestCPU t;
    // MVI A, 0x30 ; MVI B, 0x10 ; SUB B ; HLT
    t.loadCode(0, { 0x3E, 0x30, 0x06, 0x10, 0x90, 0x76 });
    t.runUntilHalt();
    ASSERT_EQ(t.cpu->GetA(), 0x20);
    ASSERT_TRUE(!t.cpu->GetFlagC());
}

static void test_sub_borrow() {
    TestCPU t;
    // MVI A, 0x00 ; MVI B, 0x01 ; SUB B ; HLT
    t.loadCode(0, { 0x3E, 0x00, 0x06, 0x01, 0x90, 0x76 });
    t.runUntilHalt();
    ASSERT_EQ(t.cpu->GetA(), 0xFF);
    ASSERT_TRUE(t.cpu->GetFlagC());
    ASSERT_TRUE(t.cpu->GetFlagS());
}

static void test_sbi() {
    TestCPU t;
    // MVI A, 0x50 ; SBI 0x10 ; HLT
    t.loadCode(0, { 0x3E, 0x50, 0xDE, 0x10, 0x76 });
    t.runUntilHalt();
    ASSERT_EQ(t.cpu->GetA(), 0x40);
}

// ─── INR / DCR (Increment / Decrement) ──────────────────────────────

static void test_inr_b() {
    TestCPU t;
    // MVI B, 0x41 ; INR B ; HLT
    t.loadCode(0, { 0x06, 0x41, 0x04, 0x76 });
    t.runUntilHalt();
    ASSERT_EQ(t.cpu->GetB(), 0x42);
}

static void test_dcr_c() {
    TestCPU t;
    // MVI C, 0x01 ; DCR C ; HLT
    t.loadCode(0, { 0x0E, 0x01, 0x0D, 0x76 });
    t.runUntilHalt();
    ASSERT_EQ(t.cpu->GetC(), 0x00);
    ASSERT_TRUE(t.cpu->GetFlagZ());
}

static void test_inr_overflow() {
    TestCPU t;
    // MVI B, 0xFF ; INR B ; HLT
    t.loadCode(0, { 0x06, 0xFF, 0x04, 0x76 });
    t.runUntilHalt();
    ASSERT_EQ(t.cpu->GetB(), 0x00);
    ASSERT_TRUE(t.cpu->GetFlagZ());
}

// ─── INX / DCX (Register Pair Inc/Dec) ──────────────────────────────

static void test_inx_bc() {
    TestCPU t;
    // LXI B, 0x00FF ; INX B ; HLT
    t.loadCode(0, { 0x01, 0xFF, 0x00, 0x03, 0x76 });
    t.runUntilHalt();
    ASSERT_EQ(t.cpu->GetBC(), 0x0100);
}

static void test_dcx_de() {
    TestCPU t;
    // LXI D, 0x0100 ; DCX D ; HLT
    t.loadCode(0, { 0x11, 0x00, 0x01, 0x1B, 0x76 });
    t.runUntilHalt();
    ASSERT_EQ(t.cpu->GetDE(), 0x00FF);
}

// ─── ANA / ORA / XRA (Logic) ────────────────────────────────────────

static void test_ana() {
    TestCPU t;
    // MVI A, 0xFF ; MVI B, 0x0F ; ANA B ; HLT
    t.loadCode(0, { 0x3E, 0xFF, 0x06, 0x0F, 0xA0, 0x76 });
    t.runUntilHalt();
    ASSERT_EQ(t.cpu->GetA(), 0x0F);
}

static void test_ora() {
    TestCPU t;
    // MVI A, 0xF0 ; MVI B, 0x0F ; ORA B ; HLT
    t.loadCode(0, { 0x3E, 0xF0, 0x06, 0x0F, 0xB0, 0x76 });
    t.runUntilHalt();
    ASSERT_EQ(t.cpu->GetA(), 0xFF);
}

static void test_xra_self() {
    TestCPU t;
    // MVI A, 0x42 ; XRA A ; HLT
    t.loadCode(0, { 0x3E, 0x42, 0xAF, 0x76 });
    t.runUntilHalt();
    ASSERT_EQ(t.cpu->GetA(), 0x00);
    ASSERT_TRUE(t.cpu->GetFlagZ());
    ASSERT_TRUE(!t.cpu->GetFlagC());
}

// ─── CMP (Compare) ──────────────────────────────────────────────────

static void test_cmp_equal() {
    TestCPU t;
    // MVI A, 0x42 ; MVI B, 0x42 ; CMP B ; HLT
    t.loadCode(0, { 0x3E, 0x42, 0x06, 0x42, 0xB8, 0x76 });
    t.runUntilHalt();
    ASSERT_TRUE(t.cpu->GetFlagZ());
    ASSERT_TRUE(!t.cpu->GetFlagC());
    ASSERT_EQ(t.cpu->GetA(), 0x42); // A unchanged
}

static void test_cmp_less() {
    TestCPU t;
    // MVI A, 0x10 ; MVI B, 0x20 ; CMP B ; HLT
    t.loadCode(0, { 0x3E, 0x10, 0x06, 0x20, 0xB8, 0x76 });
    t.runUntilHalt();
    ASSERT_TRUE(!t.cpu->GetFlagZ());
    ASSERT_TRUE(t.cpu->GetFlagC());
}

// ─── JMP / JZ / JNZ / JC / JNC (Jumps) ─────────────────────────────

static void test_jmp() {
    TestCPU t;
    // JMP 0x0010 ; (addr 0x0010:) HLT
    t.loadCode(0x0000, { 0xC3, 0x10, 0x00 });
    t.loadCode(0x0010, { 0x76 });
    t.runUntilHalt();
    ASSERT_EQ(t.cpu->GetPC(), 0x0010);
}

static void test_jz_taken() {
    TestCPU t;
    // XRA A (sets Z) ; JZ 0x0010 ; HLT ; (0x0010:) MVI A,0x42 ; HLT
    t.loadCode(0x0000, { 0xAF, 0xCA, 0x10, 0x00, 0x76 });
    t.loadCode(0x0010, { 0x3E, 0x42, 0x76 });
    t.runUntilHalt();
    ASSERT_EQ(t.cpu->GetA(), 0x42);
}

static void test_jz_not_taken() {
    TestCPU t;
    // MVI A, 0x01 ; ORA A (clears Z) ; JZ 0x0010 ; MVI A, 0x99 ; HLT
    t.loadCode(0x0000, { 0x3E, 0x01, 0xB7, 0xCA, 0x10, 0x00, 0x3E, 0x99, 0x76 });
    t.loadCode(0x0010, { 0x3E, 0x42, 0x76 });
    t.runUntilHalt();
    ASSERT_EQ(t.cpu->GetA(), 0x99);
}

// ─── CALL / RET ─────────────────────────────────────────────────────

static void test_call_ret() {
    TestCPU t;
    // LXI SP, 0x100 ; CALL 0x0010 ; MVI A, 0x99 ; HLT
    // (0x0010:) MVI A, 0x42 ; RET
    t.loadCode(0x0000, { 0x31, 0x00, 0x01, 0xCD, 0x10, 0x00, 0x3E, 0x99, 0x76 });
    t.loadCode(0x0010, { 0x3E, 0x42, 0xC9 });
    t.runUntilHalt();
    // After CALL+RET, A=0x42 from subroutine, then A=0x99 from main
    ASSERT_EQ(t.cpu->GetA(), 0x99);
}

// ─── PUSH / POP ─────────────────────────────────────────────────────

static void test_push_pop() {
    TestCPU t;
    // LXI SP, 0x100 ; LXI B, 0xABCD ; PUSH B ; POP D ; HLT
    t.loadCode(0, {
        0x31, 0x00, 0x01,  // LXI SP, 0x100
        0x01, 0xCD, 0xAB,  // LXI B, 0xABCD
        0xC5,              // PUSH B
        0xD1,              // POP D
        0x76               // HLT
    });
    t.runUntilHalt();
    ASSERT_EQ(t.cpu->GetDE(), 0xABCD);
}

// ─── RLC / RRC / RAL / RAR (Rotate) ────────────────────────────────

static void test_rlc() {
    TestCPU t;
    // MVI A, 0x80 ; RLC ; HLT
    t.loadCode(0, { 0x3E, 0x80, 0x07, 0x76 });
    t.runUntilHalt();
    ASSERT_EQ(t.cpu->GetA(), 0x01);
    ASSERT_TRUE(t.cpu->GetFlagC());
}

static void test_rrc() {
    TestCPU t;
    // MVI A, 0x01 ; RRC ; HLT
    t.loadCode(0, { 0x3E, 0x01, 0x0F, 0x76 });
    t.runUntilHalt();
    ASSERT_EQ(t.cpu->GetA(), 0x80);
    ASSERT_TRUE(t.cpu->GetFlagC());
}

// ─── STA / LDA (Direct Memory Access) ──────────────────────────────

static void test_sta_lda() {
    TestCPU t;
    // MVI A, 0x77 ; STA 0x5000 ; MVI A, 0x00 ; LDA 0x5000 ; HLT
    t.loadCode(0, {
        0x3E, 0x77,           // MVI A, 0x77
        0x32, 0x00, 0x50,     // STA 0x5000
        0x3E, 0x00,           // MVI A, 0x00
        0x3A, 0x00, 0x50,     // LDA 0x5000
        0x76                  // HLT
    });
    t.runUntilHalt();
    ASSERT_EQ(t.cpu->GetA(), 0x77);
}

// ─── DAD (Double Add) ───────────────────────────────────────────────

static void test_dad_bc() {
    TestCPU t;
    // LXI H, 0x1000 ; LXI B, 0x0234 ; DAD B ; HLT
    t.loadCode(0, {
        0x21, 0x00, 0x10,  // LXI H, 0x1000
        0x01, 0x34, 0x02,  // LXI B, 0x0234
        0x09,              // DAD B
        0x76               // HLT
    });
    t.runUntilHalt();
    ASSERT_EQ(t.cpu->GetHL(), 0x1234);
}

// ─── CPI (Compare Immediate) ────────────────────────────────────────

static void test_cpi() {
    TestCPU t;
    // MVI A, 0x42 ; CPI 0x42 ; HLT
    t.loadCode(0, { 0x3E, 0x42, 0xFE, 0x42, 0x76 });
    t.runUntilHalt();
    ASSERT_TRUE(t.cpu->GetFlagZ());
    ASSERT_TRUE(!t.cpu->GetFlagC());
}

// ─── OUT / IN ───────────────────────────────────────────────────────

static void test_out() {
    TestCPU t;
    // MVI A, 0xAB ; OUT 0x42 ; HLT
    t.loadCode(0, { 0x3E, 0xAB, 0xD3, 0x42, 0x76 });
    t.runUntilHalt();
    ASSERT_EQ(io_ports_out[0x42], 0xAB);
}

static void test_in() {
    TestCPU t;
    io_ports_in[0x10] = 0x77;
    // IN 0x10 ; HLT
    t.loadCode(0, { 0xDB, 0x10, 0x76 });
    t.runUntilHalt();
    ASSERT_EQ(t.cpu->GetA(), 0x77);
}

// ─── HLT ────────────────────────────────────────────────────────────

static void test_hlt() {
    TestCPU t;
    t.loadCode(0, { 0x76 });
    t.runUntilHalt(100);
    ASSERT_TRUE(t.cpu->GetHLTA());
    ASSERT_EQ(t.cpu->GetPC(), 0x0000);
}

// ─── EI / DI (Interrupt Enable/Disable) ─────────────────────────────

static void test_ei_di() {
    TestCPU t;
    // DI ; HLT
    t.loadCode(0, { 0xF3, 0x76 });
    t.runUntilHalt();
    ASSERT_TRUE(!t.cpu->GetINTE());
}

// ─── Clock cycle counting ───────────────────────────────────────────

static void test_nop_cycles() {
    TestCPU t;
    t.loadCode(0, { 0x00, 0x76 }); // NOP, HLT
    uint64_t ccBefore = t.cpu->GetCC();
    t.execOne(); // NOP = 1 machine cycle = 4 cc
    uint64_t ccAfter = t.cpu->GetCC();
    ASSERT_EQ(ccAfter - ccBefore, 4u);
}

static void test_mvi_cycles() {
    TestCPU t;
    t.loadCode(0, { 0x06, 0x42, 0x76 }); // MVI B,...; HLT
    uint64_t ccBefore = t.cpu->GetCC();
    t.execOne(); // MVI = 2 machine cycles = 8 cc
    uint64_t ccAfter = t.cpu->GetCC();
    // MVI r, data: 2 machine cycles × 4 = 8 clock cycles
    ASSERT_EQ(ccAfter - ccBefore, 8u);
}

// ─── XCHG ───────────────────────────────────────────────────────────

static void test_xchg() {
    TestCPU t;
    // LXI D, 0x1234 ; LXI H, 0x5678 ; XCHG ; HLT
    t.loadCode(0, {
        0x11, 0x34, 0x12,  // LXI D, 0x1234
        0x21, 0x78, 0x56,  // LXI H, 0x5678
        0xEB,              // XCHG
        0x76               // HLT
    });
    t.runUntilHalt();
    ASSERT_EQ(t.cpu->GetDE(), 0x5678);
    ASSERT_EQ(t.cpu->GetHL(), 0x1234);
}

// ─── Parity flag ────────────────────────────────────────────────────

static void test_parity_flag() {
    TestCPU t;
    // MVI A, 0x03 ; ORA A ; HLT  (0x03 = 0b00000011, 2 bits set = even parity)
    t.loadCode(0, { 0x3E, 0x03, 0xB7, 0x76 });
    t.runUntilHalt();
    ASSERT_TRUE(t.cpu->GetFlagP()); // even parity = P set
}

static void test_parity_flag_odd() {
    TestCPU t;
    // MVI A, 0x01 ; ORA A ; HLT  (0x01 = 0b00000001, 1 bit set = odd parity)
    t.loadCode(0, { 0x3E, 0x01, 0xB7, 0x76 });
    t.runUntilHalt();
    ASSERT_TRUE(!t.cpu->GetFlagP()); // odd parity = P clear
}

// ─── Sign flag ──────────────────────────────────────────────────────

static void test_sign_flag() {
    TestCPU t;
    // MVI A, 0x80 ; ORA A ; HLT
    t.loadCode(0, { 0x3E, 0x80, 0xB7, 0x76 });
    t.runUntilHalt();
    ASSERT_TRUE(t.cpu->GetFlagS());
}

// ─── STAX / LDAX ────────────────────────────────────────────────────

static void test_stax_ldax() {
    TestCPU t;
    // LXI B, 0x2000 ; MVI A, 0x55 ; STAX B ; MVI A, 0x00 ; LDAX B ; HLT
    t.loadCode(0, {
        0x01, 0x00, 0x20,  // LXI B, 0x2000
        0x3E, 0x55,         // MVI A, 0x55
        0x02,               // STAX B
        0x3E, 0x00,         // MVI A, 0x00
        0x0A,               // LDAX B
        0x76                // HLT
    });
    t.runUntilHalt();
    ASSERT_EQ(t.cpu->GetA(), 0x55);
}

// ─── CMA (Complement Accumulator) ───────────────────────────────────

static void test_cma() {
    TestCPU t;
    // MVI A, 0xAA ; CMA ; HLT
    t.loadCode(0, { 0x3E, 0xAA, 0x2F, 0x76 });
    t.runUntilHalt();
    ASSERT_EQ(t.cpu->GetA(), 0x55);
}

// ─── STC / CMC (Set/Complement Carry) ──────────────────────────────

static void test_stc() {
    TestCPU t;
    // STC ; HLT
    t.loadCode(0, { 0x37, 0x76 });
    t.runUntilHalt();
    ASSERT_TRUE(t.cpu->GetFlagC());
}

static void test_cmc() {
    TestCPU t;
    // STC ; CMC ; HLT
    t.loadCode(0, { 0x37, 0x3F, 0x76 });
    t.runUntilHalt();
    ASSERT_TRUE(!t.cpu->GetFlagC());
}

int main()
{
    test_nop();
    test_mvi_b();
    test_mvi_a();
    test_mvi_all_regs();
    test_mov_b_a();
    test_lxi_bc();
    test_lxi_sp();
    test_add_b();
    test_add_carry();
    test_adi();
    test_sub_b();
    test_sub_borrow();
    test_sbi();
    test_inr_b();
    test_dcr_c();
    test_inr_overflow();
    test_inx_bc();
    test_dcx_de();
    test_ana();
    test_ora();
    test_xra_self();
    test_cmp_equal();
    test_cmp_less();
    test_jmp();
    test_jz_taken();
    test_jz_not_taken();
    test_call_ret();
    test_push_pop();
    test_rlc();
    test_rrc();
    test_sta_lda();
    test_dad_bc();
    test_cpi();
    test_out();
    test_in();
    test_hlt();
    test_ei_di();
    test_nop_cycles();
    test_mvi_cycles();
    test_xchg();
    test_parity_flag();
    test_parity_flag_odd();
    test_sign_flag();
    test_stax_ldax();
    test_cma();
    test_stc();
    test_cmc();

    std::cout << "CPU Tests: " << tests_passed << "/" << tests_run << " passed";
    if (tests_failed > 0) {
        std::cout << " (" << tests_failed << " FAILED)";
    }
    std::cout << std::endl;
    return (tests_failed == 0) ? 0 : 1;
}
