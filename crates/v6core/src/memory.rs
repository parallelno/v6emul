/// Vector-06C memory subsystem
///
/// Memory layout:
/// - 64 KB main RAM (0x0000–0xFFFF)
/// - 8 RAM disks × 4 pages × 64 KB = 2 MB additional RAM disk storage
///
/// Total global address space: 64 KB + 8 × 256 KB = 2,113,536 bytes

/// ROM default load address
pub const ROM_LOAD_ADDR: u16 = 0x100;

/// Main memory size
pub const MEM_64K: usize = 64 * 1024;

/// Each RAM disk page is 64 KB
pub const RAM_DISK_PAGE_LEN: usize = MEM_64K;

/// Each RAM disk has 4 pages (256 KB total)
pub const RAMDISK_PAGES_MAX: usize = 4;

/// Total size of one RAM disk (4 × 64 KB = 256 KB)
pub const MEMORY_RAMDISK_LEN: usize = RAMDISK_PAGES_MAX * MEM_64K;

/// Maximum number of RAM disks
pub const RAM_DISK_MAX: usize = 8;

/// Main memory length
pub const MEMORY_MAIN_LEN: usize = MEM_64K;

/// Total global memory: main + all RAM disks
pub const MEMORY_GLOBAL_LEN: usize = MEMORY_MAIN_LEN + MEMORY_RAMDISK_LEN * RAM_DISK_MAX;

/// Bitmask for RAM mode bits in the mapping byte
pub const MAPPING_RAM_MODE_MASK: u8 = 0b1110_0000;

/// Bitmask for all mapping mode bits
pub const MAPPING_MODE_MASK: u8 = 0b1111_0000;

/// Address space type: distinguishes stack operations from regular memory access
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum AddrSpace {
    Ram = 0,
    Stack = 1,
}

/// Memory type: ROM or RAM mode
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum MemType {
    Rom = 0,
    Ram = 1,
}

/// RAM disk mapping configuration.
///
/// Bit layout of the mode byte: `EASMM_PP`
/// - bits 0-1 (PP): `page_ram` — RAM disk page (0–3) for memory-mapped mode
/// - bits 2-3 (MM): `page_stack` — RAM disk page (0–3) for stack mode
/// - bit 4 (S): `mode_stack` — enable stack mode access
/// - bit 5 (A): `mode_ram_a` — enable memory-mapped mode for 0xA000–0xDFFF
/// - bit 6 (8): `mode_ram_8` — enable memory-mapped mode for 0x8000–0x9FFF
/// - bit 7 (E): `mode_ram_e` — enable memory-mapped mode for 0xE000–0xFFFF
#[derive(Debug, Clone, Copy, Default, PartialEq, Eq)]
pub struct Mapping {
    pub data: u8,
}

impl Mapping {
    #[inline]
    pub fn page_ram(self) -> u8 {
        self.data & 0x03
    }
    #[inline]
    pub fn page_stack(self) -> u8 {
        (self.data >> 2) & 0x03
    }
    #[inline]
    pub fn mode_stack(self) -> bool {
        self.data & 0x10 != 0
    }
    #[inline]
    pub fn mode_ram_a(self) -> bool {
        self.data & 0x20 != 0
    }
    #[inline]
    pub fn mode_ram_8(self) -> bool {
        self.data & 0x40 != 0
    }
    #[inline]
    pub fn mode_ram_e(self) -> bool {
        self.data & 0x80 != 0
    }
    #[inline]
    pub fn is_enabled(self) -> bool {
        self.data & MAPPING_MODE_MASK != 0
    }
}

/// Tracks the most recent memory mapping state
#[derive(Debug, Clone, Copy, Default)]
pub struct Update {
    pub mapping: Mapping,
    pub ramdisk_idx: u8,
    pub mem_type: MemType,
}

impl Default for MemType {
    fn default() -> Self {
        MemType::Rom
    }
}

/// The Vector-06C memory subsystem
pub struct Memory {
    /// Global RAM: 64 KB main + 8 × 256 KB RAM disks
    ram: Box<[u8; MEMORY_GLOBAL_LEN]>,
    /// Optional ROM data
    rom: Vec<u8>,
    /// RAM disk mapping configurations (one per disk)
    mappings: [Mapping; RAM_DISK_MAX],
    /// Current active update state
    update: Update,
    /// Count of enabled mappings (>1 is an exception)
    mappings_enabled: u8,
}

impl Memory {
    /// Create a new Memory instance with all RAM zeroed and no ROM.
    pub fn new() -> Self {
        Self {
            ram: vec![0u8; MEMORY_GLOBAL_LEN].into_boxed_slice().try_into().unwrap(),
            rom: Vec::new(),
            mappings: [Mapping::default(); RAM_DISK_MAX],
            update: Update::default(),
            mappings_enabled: 0,
        }
    }

    /// Initialize memory (first-time). Clears main RAM, sets ROM mode, clears mappings.
    pub fn init(&mut self) {
        // Clear main 64K RAM
        self.ram[..MEM_64K].fill(0);
        self.update.mem_type = MemType::Rom;
        self.init_ramdisk_mapping();
    }

    /// Restart (reset). Switches to RAM mode, clears mappings.
    pub fn restart(&mut self) {
        self.update.mem_type = MemType::Ram;
        self.init_ramdisk_mapping();
    }

    /// Clear all RAM disk mappings.
    pub fn init_ramdisk_mapping(&mut self) {
        self.mappings = [Mapping::default(); RAM_DISK_MAX];
        self.update.mapping = Mapping::default();
        self.update.ramdisk_idx = 0;
        self.mappings_enabled = 0;
    }

    /// Load ROM data. The ROM will be read from address 0 up to its length
    /// when memory type is MemType::Rom.
    pub fn load_rom(&mut self, data: &[u8]) {
        self.rom = data.to_vec();
    }

    /// Load data into main RAM starting at the given address.
    pub fn set_ram(&mut self, addr: u16, data: &[u8]) {
        let start = addr as usize;
        let end = (start + data.len()).min(MEM_64K);
        let len = end - start;
        self.ram[start..end].copy_from_slice(&data[..len]);
    }

    /// Translate a 16-bit CPU address to a global address using current RAM disk mappings.
    pub fn get_global_addr(&self, addr: u16, addr_space: AddrSpace) -> u32 {
        let mapping = self.update.mapping;
        if !mapping.is_enabled() {
            return addr as u32;
        }

        let ramdisk_idx = self.update.ramdisk_idx as u32;

        // Stack mode: applies when mode_stack is enabled and address space is STACK
        if mapping.mode_stack() && addr_space == AddrSpace::Stack {
            // Memory-mapped mode takes precedence over stack mode for certain ranges
            if mapping.mode_ram_a() && addr >= 0xA000 && addr <= 0xDFFF {
                let page = mapping.page_ram() as u32;
                return addr as u32 + (page + 1 + ramdisk_idx * 4) * MEM_64K as u32;
            }
            if mapping.mode_ram_8() && addr >= 0x8000 && addr <= 0x9FFF {
                let page = mapping.page_ram() as u32;
                return addr as u32 + (page + 1 + ramdisk_idx * 4) * MEM_64K as u32;
            }
            if mapping.mode_ram_e() && addr >= 0xE000 {
                let page = mapping.page_ram() as u32;
                return addr as u32 + (page + 1 + ramdisk_idx * 4) * MEM_64K as u32;
            }
            // Pure stack mode
            let page = mapping.page_stack() as u32;
            return addr as u32 + (page + 1 + ramdisk_idx * 4) * MEM_64K as u32;
        }

        // Memory-mapped mode for non-stack accesses (or stack accesses without stack mode)
        if mapping.mode_ram_a() && addr >= 0xA000 && addr <= 0xDFFF {
            let page = mapping.page_ram() as u32;
            return addr as u32 + (page + 1 + ramdisk_idx * 4) * MEM_64K as u32;
        }
        if mapping.mode_ram_8() && addr >= 0x8000 && addr <= 0x9FFF {
            let page = mapping.page_ram() as u32;
            return addr as u32 + (page + 1 + ramdisk_idx * 4) * MEM_64K as u32;
        }
        if mapping.mode_ram_e() && addr >= 0xE000 {
            let page = mapping.page_ram() as u32;
            return addr as u32 + (page + 1 + ramdisk_idx * 4) * MEM_64K as u32;
        }

        addr as u32
    }

    /// Read a byte using address translation. Returns ROM data if in ROM mode and
    /// the address is within ROM bounds.
    pub fn get_byte(&self, addr: u16, addr_space: AddrSpace) -> u8 {
        let global_addr = self.get_global_addr(addr, addr_space);
        if self.update.mem_type == MemType::Rom
            && (addr as usize) < self.rom.len()
            && global_addr < MEM_64K as u32
        {
            return self.rom[addr as usize];
        }
        self.ram[global_addr as usize]
    }

    /// CPU instruction fetch. Reads a byte at the given address in RAM address space.
    pub fn cpu_read_instr(&self, addr: u16) -> u8 {
        let global_addr = self.get_global_addr(addr, AddrSpace::Ram);
        if self.update.mem_type == MemType::Rom
            && (addr as usize) < self.rom.len()
            && global_addr < MEM_64K as u32
        {
            return self.rom[addr as usize];
        }
        self.ram[global_addr as usize]
    }

    /// CPU data read with address translation.
    pub fn cpu_read(&self, addr: u16, addr_space: AddrSpace) -> u8 {
        let global_addr = self.get_global_addr(addr, addr_space);
        if self.update.mem_type == MemType::Rom
            && (addr as usize) < self.rom.len()
            && global_addr < MEM_64K as u32
        {
            return self.rom[addr as usize];
        }
        self.ram[global_addr as usize]
    }

    /// CPU data write with address translation. Writes always go to RAM.
    pub fn cpu_write(&mut self, addr: u16, value: u8, addr_space: AddrSpace) {
        let global_addr = self.get_global_addr(addr, addr_space) as usize;
        self.ram[global_addr] = value;
    }

    /// Read 4 parallel screen plane bytes at a given screen offset.
    /// Returns packed u32: byte8 << 24 | byteA << 16 | byteC << 8 | byteE
    pub fn get_screen_bytes(&self, screen_addr_offset: u16) -> u32 {
        let byte8 = self.ram[(0x8000u16.wrapping_add(screen_addr_offset)) as usize] as u32;
        let byte_a = self.ram[(0xA000u16.wrapping_add(screen_addr_offset)) as usize] as u32;
        let byte_c = self.ram[(0xC000u16.wrapping_add(screen_addr_offset)) as usize] as u32;
        let byte_e = self.ram[(0xE000u16.wrapping_add(screen_addr_offset)) as usize] as u32;
        (byte8 << 24) | (byte_a << 16) | (byte_c << 8) | byte_e
    }

    /// Direct raw read at a global address (bypasses mapping).
    pub fn get_byte_global(&self, global_addr: u32) -> u8 {
        self.ram[global_addr as usize]
    }

    /// Direct raw write at a global address (bypasses mapping).
    pub fn set_byte_global(&mut self, global_addr: u32, value: u8) {
        self.ram[global_addr as usize] = value;
    }

    /// Get a reference to the full RAM array.
    pub fn get_ram(&self) -> &[u8; MEMORY_GLOBAL_LEN] {
        &self.ram
    }

    /// Get a mutable reference to the full RAM array.
    pub fn get_ram_mut(&mut self) -> &mut [u8; MEMORY_GLOBAL_LEN] {
        &mut self.ram
    }

    /// Configure a RAM disk mapping.
    pub fn set_ramdisk_mode(&mut self, disk_idx: usize, data: u8) {
        if disk_idx >= RAM_DISK_MAX {
            return;
        }
        self.mappings[disk_idx] = Mapping { data };

        // Scan all mappings to find the first enabled one
        self.mappings_enabled = 0;
        self.update.mapping = Mapping::default();
        for (i, mapping) in self.mappings.iter().enumerate() {
            if mapping.is_enabled() {
                if self.mappings_enabled == 0 {
                    self.update.mapping = *mapping;
                    self.update.ramdisk_idx = i as u8;
                }
                self.mappings_enabled += 1;
            }
        }
    }

    /// Check if multiple RAM disks are enabled (exception condition).
    /// Returns true if >1 mapping is enabled, and resets the counter.
    pub fn is_exception(&mut self) -> bool {
        if self.mappings_enabled > 1 {
            self.mappings_enabled = 0;
            return true;
        }
        false
    }

    /// Check if ROM is currently readable.
    pub fn is_rom_enabled(&self) -> bool {
        self.update.mem_type == MemType::Rom
    }

    /// Set memory type (ROM or RAM).
    pub fn set_mem_type(&mut self, mem_type: MemType) {
        self.update.mem_type = mem_type;
    }

    /// Get current memory type.
    pub fn mem_type(&self) -> MemType {
        self.update.mem_type
    }

    /// Get a reference to the mappings array.
    pub fn mappings(&self) -> &[Mapping; RAM_DISK_MAX] {
        &self.mappings
    }
}

impl Default for Memory {
    fn default() -> Self {
        Self::new()
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn test_new_memory_zeroed() {
        let mem = Memory::new();
        for i in 0..MEM_64K {
            assert_eq!(mem.ram[i], 0, "RAM not zeroed at offset {i}");
        }
    }

    #[test]
    fn test_read_write_main_ram() {
        let mut mem = Memory::new();
        mem.restart(); // RAM mode

        mem.cpu_write(0x0000, 0x42, AddrSpace::Ram);
        assert_eq!(mem.cpu_read(0x0000, AddrSpace::Ram), 0x42);

        mem.cpu_write(0xFFFF, 0xAB, AddrSpace::Ram);
        assert_eq!(mem.cpu_read(0xFFFF, AddrSpace::Ram), 0xAB);

        mem.cpu_write(0x8000, 0xCD, AddrSpace::Ram);
        assert_eq!(mem.cpu_read(0x8000, AddrSpace::Ram), 0xCD);
    }

    #[test]
    fn test_set_ram_bulk() {
        let mut mem = Memory::new();
        mem.restart();

        let data = [1, 2, 3, 4, 5];
        mem.set_ram(0x100, &data);

        for (i, &expected) in data.iter().enumerate() {
            assert_eq!(mem.cpu_read(0x100 + i as u16, AddrSpace::Ram), expected);
        }
    }

    #[test]
    fn test_rom_read() {
        let mut mem = Memory::new();
        let rom = vec![0x31, 0x00, 0x10, 0xC3, 0x00, 0x01];
        mem.load_rom(&rom);
        mem.init(); // ROM mode

        // Should read from ROM
        assert_eq!(mem.cpu_read_instr(0x0000), 0x31);
        assert_eq!(mem.cpu_read_instr(0x0005), 0x01);

        // Beyond ROM length should read RAM (which is zeroed)
        assert_eq!(mem.cpu_read_instr(0x0010), 0x00);

        // Switch to RAM mode
        mem.set_mem_type(MemType::Ram);
        // Now should read RAM (zeroed)
        assert_eq!(mem.cpu_read_instr(0x0000), 0x00);
    }

    #[test]
    fn test_no_mapping_pass_through() {
        let mem = Memory::new();
        // With no mappings enabled, global addr should equal the 16-bit addr
        assert_eq!(mem.get_global_addr(0x0000, AddrSpace::Ram), 0x0000);
        assert_eq!(mem.get_global_addr(0x8000, AddrSpace::Ram), 0x8000);
        assert_eq!(mem.get_global_addr(0xFFFF, AddrSpace::Ram), 0xFFFF);
    }

    #[test]
    fn test_ramdisk_mapping_memory_mode() {
        let mut mem = Memory::new();
        mem.restart();

        // Enable memory-mapped mode for range A000-DFFF on disk 0, page 0
        // mode_ram_a = bit 5 = 0x20, page_ram = 0 → data = 0x20
        mem.set_ramdisk_mode(0, 0x20);

        let addr: u16 = 0xA000;
        let global = mem.get_global_addr(addr, AddrSpace::Ram);
        // Expected: addr + (page_ram(0) + 1 + ramdisk_idx(0) * 4) * 64K
        // = 0xA000 + (0 + 1 + 0) * 0x10000 = 0xA000 + 0x10000 = 0x1A000
        assert_eq!(global, 0x1A000);

        // Write to mapped address and verify via global access
        mem.cpu_write(addr, 0x77, AddrSpace::Ram);
        assert_eq!(mem.get_byte_global(global), 0x77);
    }

    #[test]
    fn test_ramdisk_mapping_stack_mode() {
        let mut mem = Memory::new();
        mem.restart();

        // Enable stack mode on disk 0, page_stack = 1 (bits 2-3)
        // mode_stack = bit 4 = 0x10, page_stack = 1 → (1 << 2) = 0x04
        // data = 0x10 | 0x04 = 0x14
        mem.set_ramdisk_mode(0, 0x14);

        let addr: u16 = 0x1000;
        let global = mem.get_global_addr(addr, AddrSpace::Stack);
        // Expected: addr + (page_stack(1) + 1 + 0 * 4) * 64K
        // = 0x1000 + (1 + 1) * 0x10000 = 0x1000 + 0x20000 = 0x21000
        assert_eq!(global, 0x21000);

        // Regular RAM access should NOT be mapped via stack mode
        let global_ram = mem.get_global_addr(addr, AddrSpace::Ram);
        assert_eq!(global_ram, 0x1000);
    }

    #[test]
    fn test_ramdisk_memory_mode_overrides_stack() {
        let mut mem = Memory::new();
        mem.restart();

        // Enable both stack mode and memory-mapped A000-DFFF
        // mode_stack = 0x10, mode_ram_a = 0x20, page_ram = 2, page_stack = 1
        // data = 0x10 | 0x20 | (1 << 2) | 2 = 0x36
        mem.set_ramdisk_mode(0, 0x36);

        // Access to A000 in stack mode should use memory-mapped mode (takes precedence)
        let addr: u16 = 0xA000;
        let global = mem.get_global_addr(addr, AddrSpace::Stack);
        // Expected: addr + (page_ram(2) + 1 + 0 * 4) * 64K
        // = 0xA000 + (2 + 1) * 0x10000 = 0xA000 + 0x30000 = 0x3A000
        assert_eq!(global, 0x3A000);
    }

    #[test]
    fn test_ramdisk_multiple_disks_exception() {
        let mut mem = Memory::new();
        mem.restart();

        mem.set_ramdisk_mode(0, 0x20); // enable disk 0
        mem.set_ramdisk_mode(1, 0x20); // enable disk 1

        assert!(mem.is_exception()); // multiple disks enabled
    }

    #[test]
    fn test_screen_bytes() {
        let mut mem = Memory::new();
        mem.restart();

        mem.cpu_write(0x8000, 0x11, AddrSpace::Ram);
        mem.cpu_write(0xA000, 0x22, AddrSpace::Ram);
        mem.cpu_write(0xC000, 0x33, AddrSpace::Ram);
        mem.cpu_write(0xE000, 0x44, AddrSpace::Ram);

        let packed = mem.get_screen_bytes(0);
        assert_eq!(packed, 0x11223344);
    }

    #[test]
    fn test_global_byte_access() {
        let mut mem = Memory::new();

        mem.set_byte_global(0x10000, 0xAB);
        assert_eq!(mem.get_byte_global(0x10000), 0xAB);

        mem.set_byte_global(0, 0xCD);
        assert_eq!(mem.get_byte_global(0), 0xCD);
    }

    #[test]
    fn test_init_clears_main_ram() {
        let mut mem = Memory::new();
        mem.ram[0x1000] = 0xFF;
        mem.init();
        assert_eq!(mem.ram[0x1000], 0x00);
    }

    #[test]
    fn test_ramdisk_8000_range() {
        let mut mem = Memory::new();
        mem.restart();

        // Enable memory-mapped mode for 0x8000–0x9FFF (mode_ram_8 = bit 6 = 0x40)
        // page_ram = 0 → data = 0x40
        mem.set_ramdisk_mode(0, 0x40);

        let addr: u16 = 0x8000;
        let global = mem.get_global_addr(addr, AddrSpace::Ram);
        // addr + (0 + 1 + 0) * 64K = 0x8000 + 0x10000 = 0x18000
        assert_eq!(global, 0x18000);

        // 0x9FFF should also be mapped
        let global2 = mem.get_global_addr(0x9FFF, AddrSpace::Ram);
        assert_eq!(global2, 0x9FFF + 0x10000);

        // 0x7FFF should NOT be mapped
        let global3 = mem.get_global_addr(0x7FFF, AddrSpace::Ram);
        assert_eq!(global3, 0x7FFF);
    }

    #[test]
    fn test_ramdisk_e000_range() {
        let mut mem = Memory::new();
        mem.restart();

        // Enable memory-mapped mode for 0xE000–0xFFFF (mode_ram_e = bit 7 = 0x80)
        // page_ram = 1 → data = 0x81
        mem.set_ramdisk_mode(0, 0x81);

        let addr: u16 = 0xE000;
        let global = mem.get_global_addr(addr, AddrSpace::Ram);
        // addr + (1 + 1 + 0) * 64K = 0xE000 + 0x20000 = 0x2E000
        assert_eq!(global, 0x2E000);

        // 0xFFFF should be mapped
        let global2 = mem.get_global_addr(0xFFFF, AddrSpace::Ram);
        assert_eq!(global2, 0xFFFF + 0x20000);
    }

    #[test]
    fn test_ramdisk_different_disk_index() {
        let mut mem = Memory::new();
        mem.restart();

        // Enable memory-mapped mode for A000-DFFF on disk 3, page 2
        // data = 0x20 | 2 = 0x22
        mem.set_ramdisk_mode(3, 0x22);

        let addr: u16 = 0xA000;
        let global = mem.get_global_addr(addr, AddrSpace::Ram);
        // addr + (page_ram(2) + 1 + ramdisk_idx(3) * 4) * 64K
        // = 0xA000 + (2 + 1 + 12) * 0x10000 = 0xA000 + 15 * 0x10000 = 0xA000 + 0xF0000 = 0xFA000
        assert_eq!(global, 0xFA000);
    }

    #[test]
    fn test_disable_ramdisk() {
        let mut mem = Memory::new();
        mem.restart();

        // Enable then disable
        mem.set_ramdisk_mode(0, 0x20);
        assert_ne!(mem.get_global_addr(0xA000, AddrSpace::Ram), 0xA000);

        mem.set_ramdisk_mode(0, 0x00);
        assert_eq!(mem.get_global_addr(0xA000, AddrSpace::Ram), 0xA000);
    }
}
