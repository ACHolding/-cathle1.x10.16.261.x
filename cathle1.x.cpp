// =============================================================================
// cathle 1.x  —  rebranded Project64 1.6 Legacy + UltraHLE engine (single TU)
// Upstream UI/layout model: https://github.com/pj64team/Project64-Legacy
// (Main.c FixMenuLang, RomBrowser, status bar) — Windows plugin host rewritten
// as portable C++20 + SDL2. Not a line-for-line dump of every .c/.cpp/.rc file.
//
// Legacy → cathle 1.x map:
//   Main.c / resource menus  →  SDL2 File/System/Options/Help shell (600×400)
//   RomBrowser.cpp           →  Good Name / Country / Size / Comments list
//   Cpu.c / Interpreter CPU.c / Interpreter Ops.c / OpCode.h
//                            →  full R4300i jump tables (PRIMARY/SPECIAL/REGIMM/
//                               COP0/COP1/COP2) matching pj64team/Project64-Legacy
//   UltraHLE OS patches      →  OP_PATCH 1–61 + OSCALL/OSPATCH (slots 0x1C/0x1D;
//                               Legacy marks those RESERVED — intentional UltraHLE)
//   DMA / Pif / Memory       →  SP/PI/SI DMA, DeviceBus, IPL3 HLE boot
//   RSP / RDP plugins        →  OSTask HLE + SoftRDP walker + VI present
//
// Build: ./cathle1.x/build-macos.sh    Run: ./cathle1.x    Test: --self-test
// =============================================================================

#include <cstdint>
#include <vector>
#include <array>
#include <unordered_map>
#include <unordered_set>
#include <string>
#include <cmath>
#include <fstream>
#include <chrono>
#include <cstring>
#include <stdexcept>
#include <iostream>
#include <sstream>
#include <algorithm>
#include <functional>
#include <optional>
#include <cstdio>
#include <cstdlib>
#include <climits>
#include <cctype>
#include <random>
#include <cerrno>
#if !defined(_WIN32)
#  include <dirent.h>
#endif

#if defined(CATHLE_HAS_SDL2)
#  include <SDL.h>
#  define CATHLE_SDL2 1
#elif __has_include(<SDL.h>)
#  include <SDL.h>
#  define CATHLE_SDL2 1
#elif __has_include(<SDL2/SDL.h>)
#  include <SDL2/SDL.h>
#  define CATHLE_SDL2 1
#endif

#if defined(_WIN32) || defined(_WIN64)
#  define CATHLE_OS_WINDOWS 1
#  ifndef NOMINMAX
#    define NOMINMAX
#  endif
#  include <windows.h>
#  include <direct.h>
#  define CATHLE_PATH_SEP '\\'
#else
#  define CATHLE_OS_UNIX 1
#  include <unistd.h>
#  include <sys/stat.h>
#  define CATHLE_PATH_SEP '/'
#endif

using u8 = uint8_t;
using u16 = uint16_t;
using u32 = uint32_t;
using u64 = uint64_t;
using s32 = int32_t;
using s64 = int64_t;

static constexpr const char* APP_NAME = "cathle 1.x";
static constexpr const char* APP_TITLE = "cathle 1.x";
static constexpr const char* APP_LEGACY = "cathle 1.x (Project64 1.6 Legacy)";
static constexpr const char* VERSION = "1.x";
static constexpr const char* APP_ABOUT =
  "cathle 1.x — N64 emulator. UI modeled on Project64 Legacy 1.6; "
  "core is UltraHLE OS HLE + VR4300 interpreter (portable SDL2).";

static constexpr size_t RDRAM_SIZE = 8 * 1024 * 1024;
static constexpr size_t RSP_DMEM_SIZE = 0x1000;
static constexpr size_t RSP_IMEM_SIZE = 0x1000;
static constexpr size_t PIF_RAM_SIZE = 0x40;
static constexpr size_t EEPROM_4K_SIZE = 0x200;
static constexpr size_t EEPROM_16K_SIZE = 0x800;
static constexpr size_t SRAM_SIZE = 0x8000;
static constexpr size_t FLASHRAM_SIZE = 0x20000;

static constexpr u64 MASK_8 = 0xFF;
static constexpr u64 MASK_16 = 0xFFFF;
static constexpr u64 MASK_32 = 0xFFFFFFFFULL;
static constexpr u64 MASK_64 = 0xFFFFFFFFFFFFFFFFULL;

static constexpr u64 N64_CPU_HZ = 93750000;
static constexpr int N64_VI_NTSC_HZ = 60;
static constexpr int N64_VI_PAL_HZ = 50;
static constexpr u64 N64_CYCLES_PER_FRAME_NTSC = N64_CPU_HZ / N64_VI_NTSC_HZ;
static constexpr u64 N64_CYCLES_PER_FRAME_PAL = N64_CPU_HZ / N64_VI_PAL_HZ;
static constexpr double FRAME_PERIOD_NTSC = 1.0 / N64_VI_NTSC_HZ;
static constexpr double FRAME_PERIOD_PAL = 1.0 / N64_VI_PAL_HZ;
static constexpr double INTERP_FRAME_BUDGET_S = FRAME_PERIOD_NTSC * 0.85;
static constexpr int INTERP_MIN_STEPS = 12000;
static constexpr int INTERP_MAX_STEPS = 200000;
static constexpr int INTERP_BOOT_STEPS = 80000;

inline u8  u8m(u64 v)  { return (u8)(v & MASK_8); }
inline u16 u16m(u64 v) { return (u16)(v & MASK_16); }
inline u32 u32m(u64 v) { return (u32)(v & MASK_32); }
inline u64 u64m(u64 v) { return v & MASK_64; }
inline s32 sign8(u64 v)  { v &= MASK_8;  return (v & 0x80) ? (s32)v - 0x100 : (s32)v; }
inline s32 sign16(u64 v) { v &= MASK_16; return (v & 0x8000) ? (s32)v - 0x10000 : (s32)v; }
inline s32 sign32(u64 v) { v &= MASK_32; return (v & 0x80000000ULL) ? (s32)(v - 0x100000000ULL) : (s32)v; }
inline s64 sign64(u64 v) {
  v &= MASK_64;
  if (v & 0x8000000000000000ULL) return (s64)(v - 0x8000000000000000ULL) - (s64)0x8000000000000000ULL;
  return (s64)v;
}
inline u64 sx8_to_64(u64 v)  { return u64m((u64)(s64)sign8(v)); }
inline u64 sx16_to_64(u64 v) { return u64m((u64)(s64)sign16(v)); }
inline u64 sx32_to_64(u64 v) { return u64m((u64)(s64)sign32(v)); }

// ── Platform (Unix / Windows) ──
inline const char* cathle_os_name() {
#if defined(CATHLE_OS_WINDOWS)
  return "windows";
#else
  return "unix";
#endif
}
inline std::string cathle_path_join(const std::string& a, const std::string& b) {
  if (a.empty()) return b;
  char sep = CATHLE_PATH_SEP;
  if (!a.empty() && (a.back() == '/' || a.back() == '\\')) return a + b;
  return a + sep + b;
}
inline bool cathle_mkdir_p(const std::string& path) {
#if defined(CATHLE_OS_WINDOWS)
  return _mkdir(path.c_str()) == 0 || errno == EEXIST;
#else
  return ::mkdir(path.c_str(), 0755) == 0 || errno == EEXIST;
#endif
}

inline u32 be32(const std::vector<u8>& data, size_t off) {
  if (off + 3 >= data.size()) return 0;
  return (u32(data[off]) << 24) | (u32(data[off+1]) << 16) | (u32(data[off+2]) << 8) | u32(data[off+3]);
}
inline void put_be32(std::vector<u8>& data, size_t off, u32 val) {
  if (off + 3 >= data.size()) return;
  data[off] = (val >> 24) & 0xFF; data[off+1] = (val >> 16) & 0xFF;
  data[off+2] = (val >> 8) & 0xFF; data[off+3] = val & 0xFF;
}
inline u64 be64(const std::vector<u8>& data, size_t off) {
  return ((u64)be32(data, off) << 32) | be32(data, off + 4);
}
inline void put_be64(std::vector<u8>& data, size_t off, u64 val) {
  put_be32(data, off, (u32)(val >> 32)); put_be32(data, off + 4, (u32)val);
}

inline u32 f32_to_bits(float v) {
  if (!std::isfinite(v)) return v > 0 ? 0x7F800000u : 0xFF800000u;
  u32 bits; std::memcpy(&bits, &v, 4);
#if __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
  // store as host bits — Python used big-endian pack but bits value is IEEE bits
#endif
  return bits;
}
inline float bits_to_f32(u32 v) { float f; std::memcpy(&f, &v, 4); return f; }
inline u64 f64_to_bits(double v) {
  if (!std::isfinite(v)) return v > 0 ? 0x7FF0000000000000ULL : 0xFFF0000000000000ULL;
  u64 bits; std::memcpy(&bits, &v, 8); return bits;
}
inline double bits_to_f64(u64 v) { double d; std::memcpy(&d, &v, 8); return d; }

// CRC32 (zlib-compatible)
static u32 crc32_zlib(const u8* data, size_t len, u32 crc = 0) {
  crc = ~crc;
  for (size_t i = 0; i < len; ++i) {
    crc ^= data[i];
    for (int k = 0; k < 8; ++k)
      crc = (crc >> 1) ^ (0xEDB88320u & (-(s32)(crc & 1)));
  }
  return ~crc;
}

// Hardware map (subset matching Python)
static constexpr u32 SP_MEM_ADDR=0x04040000, SP_DRAM_ADDR=0x04040004, SP_RD_LEN=0x04040008, SP_WR_LEN=0x0404000C;
static constexpr u32 SP_STATUS=0x04040010, SP_DMA_FULL=0x04040014, SP_DMA_BUSY=0x04040018, SP_SEMAPHORE=0x0404001C;
static constexpr u32 SP_PC=0x04080000;
static constexpr u32 SP_STATUS_HALT=0x0001, SP_STATUS_BROKE=0x0002, SP_STATUS_DMA_BUSY=0x0004, SP_STATUS_DMA_FULL=0x0008;
static constexpr u32 SP_STATUS_IO_FULL=0x0010, SP_STATUS_SSTEP=0x0020, SP_STATUS_INTR_BREAK=0x0040;
static constexpr u32 SP_CLR_HALT=0x0001, SP_SET_HALT=0x0002, SP_CLR_BROKE=0x0004, SP_CLR_INTR=0x0008, SP_SET_INTR=0x0010;
static constexpr u32 SP_CLR_SSTEP=0x0020, SP_SET_SSTEP=0x0040, SP_CLR_INTR_BREAK=0x0080, SP_SET_INTR_BREAK=0x0100;
static constexpr u32 SP_CLR_SIG0=0x0200, SP_SET_SIG0=0x0400;
static constexpr u32 DPC_START=0x04100000, DPC_END=0x04100004, DPC_CURRENT=0x04100008, DPC_STATUS=0x0410000C;
static constexpr u32 DPC_STATUS_XBUS_DMEM_DMA=0x0001, DPC_STATUS_FREEZE=0x0002, DPC_STATUS_FLUSH=0x0004;
static constexpr u32 DPC_CLR_XBUS_DMEM_DMA=0x0001, DPC_SET_XBUS_DMEM_DMA=0x0002, DPC_CLR_FREEZE=0x0004, DPC_SET_FREEZE=0x0008;
static constexpr u32 DPC_CLR_FLUSH=0x0010, DPC_SET_FLUSH=0x0020;
static constexpr u32 MI_MODE=0x04300000, MI_VERSION=0x04300004, MI_INTR=0x04300008, MI_INTR_MASK=0x0430000C;
static constexpr u32 MI_MODE_INIT=0x0080, MI_MODE_EBUS=0x0100, MI_MODE_RDRAM=0x0200;
static constexpr u32 MI_CLR_INIT=0x0080, MI_SET_INIT=0x0100, MI_CLR_EBUS=0x0200, MI_SET_EBUS=0x0400, MI_CLR_DP_INTR=0x0800;
static constexpr u32 MI_CLR_RDRAM=0x1000, MI_SET_RDRAM=0x2000;
static constexpr u32 MI_INTR_SP=0x01, MI_INTR_SI=0x02, MI_INTR_AI=0x04, MI_INTR_VI=0x08, MI_INTR_PI=0x10, MI_INTR_DP=0x20;
static constexpr u32 MI_INTR_MASK_CLR_SP=0x0001, MI_INTR_MASK_SET_SP=0x0002, MI_INTR_MASK_CLR_SI=0x0004, MI_INTR_MASK_SET_SI=0x0008;
static constexpr u32 MI_INTR_MASK_CLR_AI=0x0010, MI_INTR_MASK_SET_AI=0x0020, MI_INTR_MASK_CLR_VI=0x0040, MI_INTR_MASK_SET_VI=0x0080;
static constexpr u32 MI_INTR_MASK_CLR_PI=0x0100, MI_INTR_MASK_SET_PI=0x0200, MI_INTR_MASK_CLR_DP=0x0400, MI_INTR_MASK_SET_DP=0x0800;
static constexpr u32 VI_STATUS=0x04400000, VI_ORIGIN=0x04400004, VI_WIDTH=0x04400008, VI_INTR=0x0440000C;
static constexpr u32 VI_V_CURRENT=0x04400010, VI_BURST=0x04400014, VI_V_SYNC=0x04400018, VI_H_SYNC=0x0440001C;
static constexpr u32 VI_LEAP=0x04400020, VI_H_START=0x04400024, VI_V_START=0x04400028, VI_V_BURST=0x0440002C;
static constexpr u32 VI_X_SCALE=0x04400030, VI_Y_SCALE=0x04400034;
static constexpr u32 AI_DRAM_ADDR=0x04500000, AI_LEN=0x04500004, AI_CONTROL=0x04500008, AI_STATUS=0x0450000C;
static constexpr u32 AI_DACRATE=0x04500010, AI_BITRATE=0x04500014;
static constexpr u32 PI_DRAM_ADDR=0x04600000, PI_CART_ADDR=0x04600004, PI_RD_LEN=0x04600008, PI_WR_LEN=0x0460000C;
static constexpr u32 PI_STATUS=0x04600010, PI_DOM1_LAT=0x04600014, PI_DOM2_RLS=0x04600030;
static constexpr u32 RI_MODE=0x04700000, RI_WERROR=0x0470001C;
static constexpr u32 SI_DRAM_ADDR=0x04800000, SI_PIF_ADDR_RD=0x04800004, SI_PIF_ADDR_WR=0x04800010, SI_STATUS=0x04800018;

static constexpr int CP0_INDEX=0, CP0_RANDOM=1, CP0_ENTRYLO0=2, CP0_ENTRYLO1=3, CP0_CONTEXT=4, CP0_PAGEMASK=5, CP0_WIRED=6;
static constexpr int CP0_BADVADDR=8, CP0_COUNT=9, CP0_ENTRYHI=10, CP0_COMPARE=11, CP0_STATUS=12, CP0_CAUSE=13, CP0_EPC=14;
static constexpr int CP0_PRID=15, CP0_CONFIG=16, CP0_LLADDR=17, CP0_ERROREPC=30;
static constexpr u32 STATUS_FR=0x04000000, STATUS_IE=0x0001, STATUS_EXL=0x0002, STATUS_ERL=0x0004, STATUS_BEV=0x00400000;
static constexpr u32 STATUS_CU1=0x20000000, STATUS_CU2=0x40000000;
static constexpr u32 CAUSE_IP2=0x0400, CAUSE_IP7=0x8000, CAUSE_BD=0x80000000u;
static constexpr int FCR31_COND_BIT=23;
static constexpr u32 FCR31_CAUSE_DIVBYZERO=0x08, FCR31_CAUSE_INVALID=0x10, FCR31_CAUSE_UNIMP=0x20;
static constexpr int EXC_OVERFLOW=12, EXC_TRAP=13, EXC_FPE=15;

static constexpr int CIC_NUS_6101=0, CIC_NUS_6102=1, CIC_NUS_6103=2, CIC_NUS_6105=3, CIC_NUS_6106=4;
static constexpr int CIC_NUS_5167=5, CIC_NUS_8303=6, CIC_NUS_8401=7, CIC_NUS_DDUS=8, CIC_NUS_XENO=9, CIC_NUS_7102=10;
static constexpr int SAVE_AUTO=0, SAVE_EEPROM_4K=1, SAVE_EEPROM_16K=2, SAVE_SRAM=3, SAVE_FLASHRAM=4;
static constexpr int REGION_NTSC=0, REGION_PAL=1, REGION_MPAL=2;
static constexpr u32 EEPROM_TYPE_4K = 0x8000, EEPROM_TYPE_16K = 0xC000;
static constexpr int EEPROM_BLOCK = 8;

static std::unordered_map<u32, int> CIC_IPL3_CRC32 = {
  {0x6170A4A1u, CIC_NUS_6101}, {0x009E9EA3u, CIC_NUS_7102}, {0x90BB6CB5u, CIC_NUS_6102},
  {0x0B050EE0u, CIC_NUS_6103}, {0x98BC2C86u, CIC_NUS_6105}, {0xACC8580Au, CIC_NUS_6106},
  {0x0E018159u, CIC_NUS_8303}, {0x10C68B18u, CIC_NUS_8401}, {0xBC605D0Au, CIC_NUS_DDUS},
  {0x5C124E9Cu, CIC_NUS_5167},
};
static const std::unordered_map<int, u64> CIC_SEEDS = {
  {CIC_NUS_6101, 0x3F}, {CIC_NUS_7102, 0x3F}, {CIC_NUS_6102, 0x3F}, {CIC_NUS_6103, 0x78},
  {CIC_NUS_6105, 0x91}, {CIC_NUS_6106, 0x85}, {CIC_NUS_8303, 0xDD}, {CIC_NUS_8401, 0xDD},
  {CIC_NUS_DDUS, 0xDE}, {CIC_NUS_5167, 0xDD},
};

static std::pair<int,bool> identify_cic(const std::vector<u8>& rom) {
  if (rom.size() < 0x1000) return {CIC_NUS_6102, false};
  u32 c = crc32_zlib(rom.data() + 0x40, 0x1000 - 0x40) & (u32)MASK_32;
  auto it = CIC_IPL3_CRC32.find(c);
  if (it != CIC_IPL3_CRC32.end()) return {it->second, true};
  return {CIC_NUS_6102, false};
}
static int get_cic_chip_id(const std::vector<u8>& rom) { return identify_cic(rom).first; }

static std::pair<u32,u32> recalculate_crcs(const std::vector<u8>& data) {
  if (data.size() < 0x1000) return {0,0};
  int chip = get_cic_chip_id(data);
  u32 seed = 0xF8CA4DDC;
  if (chip == CIC_NUS_6103) seed = 0xA3886759;
  else if (chip == CIC_NUS_6105) seed = 0xDF26F436;
  else if (chip == CIC_NUS_6106) seed = 0x1FEA617A;
  u32 t1=seed,t2=seed,t3=seed,t4=seed,t5=seed,t6=seed;
  size_t limit = std::min<size_t>(0x00101000, data.size());
  bool is_6105 = chip == CIC_NUS_6105;
  for (size_t i = 0x1000; i < limit; i += 4) {
    u32 d = (i + 3 < data.size()) ? be32(data, i) : 0;
    if ((u64)t6 + d < t6) t4 += 1;
    t6 = u32m(t6 + d); t3 ^= d;
    u32 shift = d & 0x1F;
    u32 r = shift ? ((d << shift) | (d >> (32 - shift))) : d;
    t5 = u32m(t5 + r);
    if (t2 > d) t2 ^= r; else t2 ^= t6 ^ d;
    if (is_6105) {
      u32 jd = (0x0753 + (i & 0xFF) < data.size()) ? be32(data, 0x0750 + (i & 0xFF)) : 0;
      t1 = u32m(t1 + (jd ^ d));
    } else t1 = u32m(t1 + (t5 ^ d));
  }
  u32 crc0, crc1;
  if (chip == CIC_NUS_6103) { crc0 = u32m((t6 ^ t4) + t3); crc1 = u32m((t5 ^ t2) + t1); }
  else if (chip == CIC_NUS_6106) { crc0 = u32m((t6 * t4) + t3); crc1 = u32m((t5 * t2) + t1); }
  else { crc0 = u32m(t6 ^ t4 ^ t3); crc1 = u32m(t5 ^ t2 ^ t1); }
  return {crc0, crc1};
}

static const u8 Z64_MAGIC[4] = {0x80,0x37,0x12,0x40};
static const u8 V64_MAGIC[4] = {0x37,0x80,0x40,0x12};
static const u8 N64_LE_MAGIC[4] = {0x40,0x12,0x37,0x80};

static bool magic_eq(const std::vector<u8>& d, size_t off, const u8 m[4]) {
  return off + 4 <= d.size() && d[off]==m[0] && d[off+1]==m[1] && d[off+2]==m[2] && d[off+3]==m[3];
}

static void strip_documentation_header(std::vector<u8>& data) {
  if (data.size() < 4) return;
  if (magic_eq(data, 0, Z64_MAGIC) || magic_eq(data, 0, V64_MAGIC) || magic_eq(data, 0, N64_LE_MAGIC)) return;
  size_t cap = std::min(data.size(), (size_t)(16 * 1024 * 1024));
  for (size_t off : {size_t(4096), size_t(2048), size_t(512)}) {
    if (off + 4 <= cap && (magic_eq(data, off, Z64_MAGIC) || magic_eq(data, off, V64_MAGIC) || magic_eq(data, off, N64_LE_MAGIC))) {
      data.erase(data.begin(), data.begin() + (std::ptrdiff_t)off);
      return;
    }
  }
}

static void apply_cart_header_defaults(std::vector<u8>& data) {
  if (data.size() < 0x40) data.resize(0x40, 0);
  if (!magic_eq(data, 0, Z64_MAGIC)) return;
  if (be32(data, 0x04) == 0) put_be32(data, 0x04, 0x00000F48);
  u32 boot = be32(data, 0x08);
  if (boot == 0 || boot == (u32)MASK_32) put_be32(data, 0x08, 0x80000400);
  if (be32(data, 0x0C) == 0) put_be32(data, 0x0C, 0x0000144B);
  bool any = false; for (int i = 0x20; i < 0x34; ++i) if (data[i]) any = true;
  if (!any) {
    const char* t = "Ultra 64";
    for (int i = 0; i < 20; ++i) data[0x20 + i] = (i < 8) ? (u8)t[i] : 0;
  }
  if (be32(data, 0x10) == 0 && be32(data, 0x14) == 0) {
    auto [c1, c2] = recalculate_crcs(data);
    put_be32(data, 0x10, c1); put_be32(data, 0x14, c2);
  }
}

static std::vector<u8> normalize_rom_bytes(std::vector<u8> data) {
  strip_documentation_header(data);
  if (data.size() < 4) return data;
  if (magic_eq(data, 0, V64_MAGIC)) {
    for (size_t i = 0; i + 1 < data.size(); i += 2) std::swap(data[i], data[i+1]);
    apply_cart_header_defaults(data); return data;
  }
  if (magic_eq(data, 0, N64_LE_MAGIC)) {
    for (size_t i = 0; i + 3 < data.size(); i += 4) {
      std::swap(data[i], data[i+3]); std::swap(data[i+1], data[i+2]);
    }
    apply_cart_header_defaults(data); return data;
  }
  if (magic_eq(data, 0, Z64_MAGIC)) { apply_cart_header_defaults(data); return data; }
  return data;
}

static std::vector<u8> read_rom_file(const std::string& path, size_t limit = 0) {
  std::ifstream f(path, std::ios::binary);
  if (!f) throw std::runtime_error("cannot open ROM: " + path);
  f.seekg(0, std::ios::end); size_t sz = (size_t)f.tellg(); f.seekg(0);
  if (limit) sz = std::min(sz, limit);
  std::vector<u8> data(sz);
  if (sz) f.read((char*)data.data(), (std::streamsize)sz);
  return data;
}

static void seed_pif_ram(std::vector<u8>& pif, int cic) {
  pif.assign(PIF_RAM_SIZE, 0);
  for (int i = 0; i < 4; ++i) pif[i * 4] = 0x01;
  pif[0x18] = 0x00; pif[0x19] = 0x04;
  u8 cb[4] = {0,0,0,0};
  if (cic == CIC_NUS_6101 || cic == CIC_NUS_7102) { cb[0]=0x00; cb[1]=0x06; cb[2]=0x3F; cb[3]=0x3F; }
  else if (cic == CIC_NUS_6102) { cb[0]=0x00; cb[1]=0x02; cb[2]=0x3F; cb[3]=0x3F; }
  else if (cic == CIC_NUS_6103) { cb[0]=0x00; cb[1]=0x02; cb[2]=0x78; cb[3]=0x3F; }
  else if (cic == CIC_NUS_6105) { cb[0]=0x00; cb[1]=0x02; cb[2]=0x91; cb[3]=0x3F; }
  else if (cic == CIC_NUS_6106) { cb[0]=0x00; cb[1]=0x02; cb[2]=0x85; cb[3]=0x3F; }
  else return;
  pif[36]=cb[0]; pif[37]=cb[1]; pif[38]=cb[2]; pif[39]=cb[3];
}

static std::vector<int> cic_6105_response(const std::vector<int>& challenge) {
  static const int lut0[16] = {0x4,0x7,0xA,0x7,0xE,0x5,0xE,0x1,0xC,0xF,0x8,0xF,0x6,0x3,0x6,0x9};
  static const int lut1[16] = {0x4,0x1,0xA,0x7,0xE,0x5,0xE,0x1,0xC,0x9,0x8,0x5,0x6,0x3,0xC,0x9};
  int key = 0xB; const int* lut = lut0; std::vector<int> out;
  for (int c : challenge) {
    int r = (key + 5 * c) & 0xF;
    out.push_back(r);
    key = lut[r];
    int sgn = (r >> 3) & 1;
    int mag = ((sgn ? ~r : r) & 0x7);
    int mod = (mag % 3 == 1) ? sgn : (1 - sgn);
    if (lut == lut1 && (r == 0x1 || r == 0x9)) mod = 1;
    if (lut == lut1 && (r == 0xB || r == 0xE)) mod = 0;
    lut = (mod == 1) ? lut1 : lut0;
  }
  return out;
}

static int pak_address_crc(int addr) {
  int ret = 0, bit = 0x400;
  while (bit) {
    ret <<= 1;
    if (addr & bit) ret = (ret & 0x20) ? ret ^ 0x14 : ret + 1;
    else if (ret & 0x20) ret ^= 0x15;
    bit >>= 1;
  }
  for (int i = 0; i < 5; ++i) { ret <<= 1; if (ret & 0x20) ret ^= 0x15; }
  return ret & 0x1F;
}
static int pak_data_crc(const u8* data) {
  int ret = 0;
  for (int i = 0; i < 32; ++i) {
    int j = 0x80;
    while (j) {
      ret <<= 1;
      if (data[i] & j) ret = (ret & 0x100) ? ret ^ 0x84 : ret + 1;
      else if (ret & 0x100) ret ^= 0x85;
      j >>= 1;
    }
  }
  for (int i = 0; i < 8; ++i) { ret <<= 1; if (ret & 0x100) ret ^= 0x85; }
  return ret & 0xFF;
}

static int get_rom_region(const std::vector<u8>& rom) {
  if (rom.size() < 0x3F) return REGION_NTSC;
  u8 c = rom[0x3E];
  if (c == 0x42) return REGION_MPAL;
  for (u8 x : {u8(0x44),u8(0x46),u8(0x49),u8(0x50),u8(0x53),u8(0x55),u8(0x58),u8(0x59)})
    if (c == x) return REGION_PAL;
  return REGION_NTSC;
}

struct GameInfo { std::string name; int save = SAVE_AUTO; bool ram8 = false; };
static GameInfo game_info(const std::vector<u8>& rom) {
  if (rom.size() < 0x40) return {};
  char code[3] = {(char)rom[0x3C], (char)rom[0x3D], 0};
  std::string cart = code;
  for (char& ch : cart) ch = (char)toupper((unsigned char)ch);
  static const std::unordered_map<std::string, GameInfo> DB = {
    {"SM", {"Super Mario 64", SAVE_EEPROM_4K}}, {"ZL", {"OoT", SAVE_SRAM}},
    {"ZS", {"MM", SAVE_FLASHRAM, true}}, {"KT", {"MK64", SAVE_EEPROM_4K}},
    {"YS", {"Yoshi", SAVE_EEPROM_16K}}, {"PD", {"Perfect Dark", SAVE_EEPROM_16K}},
    {"BK", {"Banjo", SAVE_EEPROM_4K}}, {"DO", {"DK64", SAVE_EEPROM_16K, true}},
  };
  auto it = DB.find(cart); return it == DB.end() ? GameInfo{} : it->second;
}

static int detect_save_type(const std::vector<u8>& rom) {
  if (rom.size() < 0x40) return SAVE_AUTO;
  auto info = game_info(rom);
  if (info.save != SAVE_AUTO) return info.save;
  char cart[3] = {(char)toupper(rom[0x3C]), (char)toupper(rom[0x3D]), 0};
  std::string c = cart;
  static const std::unordered_set<std::string> eeprom4 = {
    "SM","GE","NE","KT","SV","WR","RC","BM","BE","TW","IR","GU","CL","IC","LB","MW","SI","SQ","TE","TM","VG","VT","WA"};
  static const std::unordered_set<std::string> eeprom16 = {"ZS","DZ","FU","PG","CZ","DL","DO","JD","YS","PD","B7","MV","M8"};
  static const std::unordered_set<std::string> sram = {"ZL","TP","YW","NB","MZ","CF","DK","FZ","KI","OS","WC","YF","AL","MF","K4","OB","RE"};
  static const std::unordered_set<std::string> flash = {"PO","MX","DQ","PN","CC","CR","DR","ML","AF","MQ"};
  // Note: YS/PD are in GAME_DB as E16; flash set from Python heuristic differs for YS — use GAME_DB first.
  if (flash.count(c)) return SAVE_FLASHRAM;
  if (sram.count(c)) return SAVE_SRAM;
  if (eeprom16.count(c)) return SAVE_EEPROM_16K;
  if (eeprom4.count(c)) return SAVE_EEPROM_4K;
  return SAVE_EEPROM_4K;
}

struct N64Header {
  u32 clock_rate = 0, boot_address = 0x80000400, release = 0, crc1 = 0, crc2 = 0;
  std::string title = "UNKNOWN", cart_id = "??";
  N64Header() = default;
  explicit N64Header(const std::vector<u8>& data) {
    if (data.size() >= 0x40) {
      clock_rate = be32(data, 0x04); boot_address = be32(data, 0x08); release = be32(data, 0x0C);
      crc1 = be32(data, 0x10); crc2 = be32(data, 0x14);
      title.clear();
      for (int i = 0x20; i < 0x34; ++i) if (data[i]) title.push_back((char)data[i]);
      while (!title.empty() && title.back() == ' ') title.pop_back();
      cart_id = std::string(1, (char)data[0x3C]) + std::string(1, (char)data[0x3D]);
    }
  }
};

struct TLBEntry {
  u32 mask = 0, vpn2 = 0, asid = 0, pfn0 = 0, pfn1 = 0;
  int c0 = 0, c1 = 0;
  bool g = false, d0 = false, v0 = false, d1 = false, v1 = false;
};

static constexpr u32 _ID_SPECIAL=0x00000, _ID_REGIMM=0x10000, _ID_COP0_RS=0x20000, _ID_COP0_CO=0x30000;
static constexpr u32 _ID_COP1_RS=0x40000, _ID_COP1_BC=0x50000, _ID_FPU=0x60000, _ID_PRIMARY=0x70000;
static constexpr int _ID_FPU_S=0, _ID_FPU_D=1, _ID_FPU_W=2, _ID_FPU_L=3;

static constexpr u32 ULTRAHLE_OP_PATCH = 0x1C, ULTRAHLE_OP_GROUP = 0x1D;
static constexpr u32 ULTRAHLE_OS_SEG = 0x03FF0000;
static constexpr int _UH_V0=2, _UH_V1=3, _UH_A0=4, _UH_A1=5, _UH_A2=6, _UH_A3=7, _UH_SP=29, _UH_RA=31;
static constexpr int OS_EVENT_SW1=0, OS_EVENT_SW2=1, OS_EVENT_CART=2, OS_EVENT_COUNTER=3;
static constexpr int OS_EVENT_SP=4, OS_EVENT_SI=5, OS_EVENT_AI=6, OS_EVENT_VI=7, OS_EVENT_PI=8, OS_EVENT_DP=9;
static constexpr int OS_EVENT_PRENMI=14, OS_EVENT_RETRACE=15;
static constexpr int OS_MESG_NOBLOCK=0, OS_MESG_BLOCK=1;
static constexpr u32 ULTRAHLE_THREAD_RA = 0x03FF0000u;
static constexpr u32 ULTRAHLE_MQ_MAGIC = 0xFCFC1234u;
static constexpr int ULTRAHLE_MAX_THREAD = 16;
static constexpr int ULTRAHLE_MAX_QUEUE = 128;
static constexpr u32 ULTRAHLE_CODESIZE = 0x100000u;
static constexpr u32 ADDRMASK_UH = 0x7FFFFFFFu;

inline u32 make_ultrahle_patch(int patch_id) {
  return u32m((ULTRAHLE_OP_PATCH << 26) | (patch_id & 0xFFFF));
}

static u32 normalize_commercial_entry(u32 addr) {
  addr = u32m(addr);
  if (addr == 0 || addr == (u32)MASK_32) return 0x80000400;
  u32 hi = addr >> 24;
  if (hi == 0x80 || hi == 0xA0 || hi == 0xB0) {
    if (hi == 0xB0) return 0x80000000 | (addr & 0x1FFFFFFF);
    return addr;
  }
  if (addr < RDRAM_SIZE) return 0x80000000 | addr;
  if (hi == 0 && addr < 0x04000000) return 0x80000000 | addr;
  return addr;
}

struct N64Opcode {
  u32 word = 0, op = 0, rs = 0, rt = 0, rd = 0, sa = 0, funct = 0, imm = 0, target = 0, instr_id = 0;
  s32 simm = 0;
  N64Opcode() = default;
  explicit N64Opcode(u32 w) {
    word = w & (u32)MASK_32;
    op = (word >> 26) & 0x3F; rs = (word >> 21) & 0x1F; rt = (word >> 16) & 0x1F;
    rd = (word >> 11) & 0x1F; sa = (word >> 6) & 0x1F; funct = word & 0x3F;
    imm = word & (u32)MASK_16; simm = sign16(imm); target = word & 0x03FFFFFF;
    if (op == 0) instr_id = _ID_SPECIAL | funct;
    else if (op == 1) instr_id = _ID_REGIMM | rt;
    // COP0 CO bit is rs[4]; any rs with that bit set is a CO-format op (funct in low 6).
    else if (op == 0x10) instr_id = (rs & 0x10) ? (_ID_COP0_CO | funct) : (_ID_COP0_RS | rs);
    else if (op == 0x11) {
      if (rs == 0x10) instr_id = _ID_FPU | (_ID_FPU_S << 6) | funct;
      else if (rs == 0x11) instr_id = _ID_FPU | (_ID_FPU_D << 6) | funct;
      else if (rs == 0x14) instr_id = _ID_FPU | (_ID_FPU_W << 6) | funct;
      else if (rs == 0x15) instr_id = _ID_FPU | (_ID_FPU_L << 6) | funct;
      else if (rs == 0x08) instr_id = _ID_COP1_BC | rt;
      else instr_id = _ID_COP1_RS | rs;
    } else instr_id = _ID_PRIMARY | op;
  }
  u32 target_addr(u32 pc) const { return u32m(((pc + 4) & 0xF0000000) | (target << 2)); }
  u32 branch_addr(u32 pc) const { return u32m(pc + 4 + (simm << 2)); }
};

static std::unordered_map<u32, N64Opcode> _OPCODE_CACHE;
static constexpr size_t _OPCODE_CACHE_MAX = 8192;
static const N64Opcode& get_opcode(u32 word) {
  word &= (u32)MASK_32;
  auto it = _OPCODE_CACHE.find(word);
  if (it != _OPCODE_CACHE.end()) return it->second;
  N64Opcode o(word);
  if (_OPCODE_CACHE.size() < _OPCODE_CACHE_MAX) {
    auto [ins, _] = _OPCODE_CACHE.emplace(word, o);
    return ins->second;
  }
  // fallback: return static temporary via thread_local
  thread_local N64Opcode tmp;
  tmp = o;
  return tmp;
}

struct TLBException {
  int code; u32 vaddr; bool refill;
  TLBException(int c, u32 v, bool r) : code(c), vaddr(v), refill(r) {}
};

// Forward decls
struct ACsN64Core;
struct CPUCore;
struct UltraHleOs;
struct DeviceBus;

using HandlerFn = void(*)(CPUCore*, const N64Opcode&, u32, u64*);
static constexpr size_t DISPATCH_SIZE = (_ID_PRIMARY | 0x3F) + 1;
static HandlerFn _DISPATCH[DISPATCH_SIZE];

using UhPatchFn = const char*(*)(UltraHleOs*, CPUCore*, u64*);
static std::unordered_map<int, UhPatchFn> ULTRAHLE_PATCH_TABLE;
static std::unordered_map<int, std::string> ULTRAHLE_PATCH_NAMES;



// ── SaveManager ──
struct SaveManager {
  int save_type = SAVE_AUTO;
  std::vector<u8> eeprom, sram, flashram;
  bool dirty = false;
  u64 flash_status = 0x1111800100C20000ULL;
  int flashram_mode = 0;
  SaveManager() { reset(); }
  void reset() {
    eeprom.assign(EEPROM_16K_SIZE, 0);
    sram.assign(SRAM_SIZE, 0);
    flashram.assign(FLASHRAM_SIZE, 0xFF);
    dirty = false; flashram_mode = 0; flash_status = 0x1111800100C20000ULL;
  }
  bool eeprom_present() const { return save_type == SAVE_AUTO || save_type == SAVE_EEPROM_4K || save_type == SAVE_EEPROM_16K; }
  size_t eeprom_capacity() const {
    if (save_type == SAVE_EEPROM_16K) return EEPROM_16K_SIZE;
    if (save_type == SAVE_EEPROM_4K || save_type == SAVE_AUTO) return EEPROM_4K_SIZE;
    return 0;
  }
  int eeprom_probe() const {
    if (!eeprom_present()) return 0;
    return save_type == SAVE_EEPROM_16K ? (int)EEPROM_TYPE_16K : (int)EEPROM_TYPE_4K;
  }
  int eeprom_read_block(int block, u8* out) {
    size_t cap = eeprom_capacity(); if (!cap) return -1;
    size_t addr = (size_t)block * EEPROM_BLOCK;
    if (addr + EEPROM_BLOCK > cap) return -1;
    std::memcpy(out, eeprom.data() + addr, EEPROM_BLOCK); return 0;
  }
  int eeprom_write_block(int block, const u8* data) {
    size_t cap = eeprom_capacity(); if (!cap) return -1;
    size_t addr = (size_t)block * EEPROM_BLOCK;
    if (addr + EEPROM_BLOCK > cap) return -1;
    std::memcpy(eeprom.data() + addr, data, EEPROM_BLOCK); dirty = true; return 0;
  }
  u32 cart2_read32(u32 off) {
    if (save_type == SAVE_FLASHRAM) return (u32)(flash_status >> 32);
    if (save_type == SAVE_SRAM && off + 4 <= sram.size()) return be32(sram, off);
    return 0;
  }
  void cart2_write32(u32 off, u32 val) {
    if (save_type == SAVE_SRAM && off + 4 <= sram.size()) { put_be32(sram, off, val); dirty = true; }
  }
  void pi_read(u32 ca, int ln, std::vector<u8>& rdram, u32 da) {
    u32 off = (ca - 0x08000000) & 0x07FFFFFF;
    ln = std::max(0, std::min(ln, (int)rdram.size() - (int)da));
    if (save_type == SAVE_SRAM) {
      u32 o = off & 0x3FFFF;
      int n = std::min(ln, (int)sram.size() - (int)o);
      if (n > 0) std::memcpy(rdram.data() + da, sram.data() + o, n);
    }
  }
  void pi_write(u32 ca, int ln, const std::vector<u8>& rdram, u32 da) {
    u32 off = (ca - 0x08000000) & 0x07FFFFFF;
    if (save_type == SAVE_SRAM) {
      u32 o = off & 0x3FFFF;
      ln = std::max(0, std::min(ln, (int)sram.size() - (int)o));
      if (ln > 0) { std::memcpy(sram.data() + o, rdram.data() + da, ln); dirty = true; }
    }
  }
};

// ── Minimal HLE subsystems (enough for core construct / process paths) ──
struct SoftRDP {
  ACsN64Core* core = nullptr;
  bool fs_skip = false;
  bool fb_ready = false; // color image cleared/filled — safe to present (PJ64 plugin-style)
  u32 cimg = 0, cwidth = 320, cfmt = 2, csiz = 2; // RGBA5551 default
  u16 fill_color = 0;
  u64 pixels_drawn = 0;
  std::unordered_map<std::string, int> stats;
  SoftRDP(ACsN64Core* c = nullptr) : core(c) { reset(); }
  void reset() {
    fs_skip = false; fb_ready = false; cimg = 0; cwidth = 320; cfmt = 2; csiz = 2; fill_color = 0; pixels_drawn = 0;
    stats = {{"tris",0},{"pixels",0},{"cmds",0},{"dls",0}};
  }
  void clear_cimg(u16 color);
  void fill_rect(u32 x0, u32 y0, u32 x1, u32 y1, u16 color, bool count_pixels = true);
  void present_buffer(u32 phys, int width, int height, u16 color);
};
struct GfxHLE {
  ACsN64Core* core = nullptr; SoftRDP* rdp = nullptr;
  std::array<u32,16> segments{};
  GfxHLE(ACsN64Core* c=nullptr, SoftRDP* r=nullptr) : core(c), rdp(r) {}
  void reset() { segments.fill(0); segments[0] = 0; }
  u32 resolve(u32 addr) const;
  void walk_dl(u32 ptr, int depth);
  void run_task(u32 data_ptr, const std::string& = "");
};
struct AudioHLE {
  ACsN64Core* core = nullptr;
  AudioHLE(ACsN64Core* c=nullptr) : core(c) {}
  void reset() {}
  void use_rom_tables(const std::vector<u8>&) {}
  void run_abi1(u32, u32) {}
};
struct RSP {
  ACsN64Core* core = nullptr;
  u32 pc = 0;
  RSP(ACsN64Core* c=nullptr) : core(c) {}
  void reset() { pc = 0; }
};
struct BlockJIT {
  ACsN64Core* core = nullptr;
  std::unordered_map<u32, int> page_blocks;
  std::unordered_map<u32, int> blocks;
  BlockJIT(ACsN64Core* c=nullptr) : core(c) {}
  void clear() { page_blocks.clear(); blocks.clear(); }
  void invalidate_page(u32) {}
  void invalidate_mapped() {}
  void invalidate_range(u32, u32) {}
  int compile(u32) { return 0; } // interpreter-only stub
};

struct UhThread {
  int id = 0; u32 memaddr = 0; int active = 0, ready = 0, pri = 0;
  int recvblock = 0, sendblock = 0;
  u32 pc = 0, next_pc = 4; u64 hi = 0, lo = 0;
  std::array<u64,32> gpr{};
};
struct UhQueue {
  u32 memaddr = 0, msg = 0; int size = 1, num = 0, pos = 0;
  std::vector<u32> data;
};
struct UhTimer {
  bool active = false; u32 expire = 0, interval = 0, mq = 0, msg = 0;
};

struct UltraHleOs {
  ACsN64Core* core = nullptr;
  std::unordered_map<int, UhQueue> queues;
  std::unordered_map<u32, int> queue_by_addr;
  int queuenum = 1;
  std::unordered_map<int, u32> event_mq, event_msg;
  u32 fb_current = 0, fb_next = 0;
  std::unordered_map<int, UhThread> threads;
  std::unordered_map<u32, int> thread_by_addr;
  int current_thread = 0, threadnum = 1;
  std::unordered_map<u32, UhTimer> timers;
  bool sptaskload = false;
  u32 time_lo = 0, time_hi = 0;
  u32 cont_status = 0x0500, cont_pad = 0;
  u32 ai_freq = 32000, ai_buf = 0, ai_len = 0;
  int ismario = 0, iszelda = 0, bootloader = 0, block_pc = 0, pending_switch = 0, preempt_tid = 0;

  UltraHleOs(ACsN64Core* c=nullptr) : core(c) { reset(); }
  void reset();
  DeviceBus* _bus();
  u32 _rd32(u32 addr);
  void _wr32(u32 addr, u32 val);
  u32 _sp_arg(u64* g, int n);
  void install_patch(u32 vaddr, int patch_id);
  void _save_thread_cpu(int tid);
  void _load_thread_cpu(int tid);
  int _find_ready_thread(int prefer = 0);
  void switch_to(int tid);
  bool schedule(int prefer = 0);
  void block_current(int reason_q = 0, bool send = false);
  void unblock_waiters(int qid, bool recv = true);
  void _request_preempt(int tid);
  bool maybe_preempt();
  bool yield_fair();
  void _queue_to_mem(int qid);
  void os_create_mesg_queue(u32 mq, u32 msg, int count);
  int _qid_from_mq(u32 mq);
  int os_send_mesg(u32 mq, u32 mesg, int block);
  int os_recv_mesg(u32 mq, u32 msg_ptr, int block);
  void os_set_event_mesg(int event, u32 mq, u32 mesg);
  void os_event(int event);
  void tick_time(u64 cycles = 1);
  void _fire_timers(u32 count);
  u32 virt_to_phys(u32 addr);
  u32 phys_to_virt(u32 addr);
  void create_thread(u32 m_thread, u32 tid, u32 entry, u32 arg, u32 stack, u32 pri);
  void start_thread(u32 m_thread);
  void stop_current_thread();
};

static inline int _uh_op_op(u32 x) { return (int)((x >> 26) & 0x3F); }
static inline u32 _uh_op_imm(u32 x) { return x & 0xFFFFu; }

struct UhSymEntry {
  u32 addr = 0; std::string text; int patch = 0; u32 original = 0;
};

struct UltraHleSym {
  ACsN64Core* core = nullptr;
  std::vector<UhSymEntry> syms;
  std::unordered_map<u32, std::pair<std::string,int>> found;
  std::unordered_set<u32> yield_addrs;
  int patches_applied = 0;
  u32 first_patch = 0, last_patch = 0;
  u32 osrange0 = 0, osrange1 = 0;
  int ismario = 0, iszelda = 0, bootloader = 0;
  std::vector<std::tuple<int,u32,std::string,u32>> ini_patches;
  u32 codebase = 0x80000400, codesize = ULTRAHLE_CODESIZE;
  std::string last_report;
  UltraHleSym(ACsN64Core* c=nullptr) : core(c) { reset(); }
  void reset() {
    syms.clear(); syms.push_back({0, "(null)", 0, 0});
    found.clear(); yield_addrs.clear();
    patches_applied = 0; first_patch = last_patch = 0;
    osrange0 = osrange1 = 0; ismario = iszelda = bootloader = 0;
    ini_patches.clear(); codebase = 0x80000400; codesize = ULTRAHLE_CODESIZE;
    last_report.clear();
  }
  u32 _rd32(u32 addr);
  void _wr32(u32 addr, u32 val);
  bool _disabled(const std::string& name) const;
  int add_symbol(u32 addr, const std::string& text, int patch);
  std::string find_symbol_name(u32 addr) const;
  std::pair<u32,u32> routine_crc2(u32 addr, u32 crc1_in);
  void patch_names();
  std::string find_os_calls(u32 base, u32 nbytes);
  int add_patches();
  std::string find_first_os();
  void apply_game_profile(const std::string& title);
  int apply_ini_patches(int dma_num = 0);
  std::string boot_scan_and_patch(const std::string& title, u32 codebase_in);
};

struct CheatEngine {
  void apply(DeviceBus&, std::vector<u8>&) {}
};

struct UhIniPatch { int when; u32 addr; const char* kind; u32 data; };
struct UhIniProfile {
  const char* key;
  int ismario, iszelda, bootloader;
  u32 osrange0, osrange1;
  const UhIniPatch* patches;
  int npatches;
};
static const UhIniPatch UH_PATCH_ZELDA[] = {
  {0, 0x80005BA0u, "patch", 2}, {0, 0x80003500u, "patch", 2}, {0, 0x800012A0u, "patch", 2},
  {0, 0x80001600u, "patch", 2}, {0, 0x80005130u, "patch", 2}, {-1, 0x8011B9D9u, "byte", 1},
};
static const UhIniPatch UH_PATCH_BANJO[] = {
  {0, 0x8000052Cu, "patch", 49}, {0, 0x80000530u, "word", 0x0320F809u}, {0, 0x80000534u, "word", 0x8FA40020u},
};
static const UhIniProfile ULTRAHLE_INI_PROFILES[] = {
  {"SUPER MARIO", 1,0,0, 0x80300000u, 0x80380000u, nullptr, 0},
  {"THE LEGEND ", 0,1,0, 0x80001000u, 0x80008000u, UH_PATCH_ZELDA, 6},
  {"ZELDA", 0,1,0, 0x80001000u, 0x80010000u, nullptr, 0},
  {"Wave Race", 0,0,0, 0x800C0000u, 0x800F0000u, nullptr, 0},
  {"WAVE RACE", 0,0,0, 0x800C0000u, 0x800F0000u, nullptr, 0},
  {"Banjo-Kazooie", 0,0,1, 0x80000000u, 0x80010000u, UH_PATCH_BANJO, 3},
  {"BANJO", 0,0,1, 0x80000000u, 0x80010000u, UH_PATCH_BANJO, 3},
  {"GoldenEye", 0,0,0, 0x80008000u, 0x80024000u, nullptr, 0},
  {"GOLDENEYE", 0,0,0, 0x80008000u, 0x80024000u, nullptr, 0},
  {"MARIOKART", 0,0,0, 0x80000000u, 0x80100000u, nullptr, 0},
  {"MARIO KART", 0,0,0, 0x80000000u, 0x80100000u, nullptr, 0},
  {"STAR FOX", 0,0,0, 0x80000000u, 0x80100000u, nullptr, 0},
  {"STARFOX", 0,0,0, 0x80000000u, 0x80100000u, nullptr, 0},
  {"F-ZERO", 0,0,1, 0x80000000u, 0x80100000u, nullptr, 0},
  {"DONKEY KONG", 0,0,0, 0x80000000u, 0x80100000u, nullptr, 0},
  {"PAPER MARIO", 0,0,0, 0x80000000u, 0x80100000u, nullptr, 0},
  {"SMASH BROS", 0,0,0, 0x80000000u, 0x80100000u, nullptr, 0},
  {"DOOM", 0,0,0, 0x80000000u, 0x80100000u, nullptr, 0},
  {"QUAKE", 0,0,0, 0x80000000u, 0x80100000u, nullptr, 0},
};
static constexpr size_t ULTRAHLE_INI_PROFILES_N = sizeof(ULTRAHLE_INI_PROFILES)/sizeof(ULTRAHLE_INI_PROFILES[0]);

static const UhIniProfile* ultrahle_find_profile(const std::string& title) {
  std::string t = title;
  for (char& c : t) c = (char)toupper((unsigned char)c);
  for (size_t i=0;i<ULTRAHLE_INI_PROFILES_N;i++) {
    std::string k = ULTRAHLE_INI_PROFILES[i].key;
    for (char& c : k) c = (char)toupper((unsigned char)c);
    if (t.rfind(k, 0) == 0 || t.find(k) != std::string::npos) return &ULTRAHLE_INI_PROFILES[i];
  }
  return nullptr;
}
static std::unordered_map<std::string, int> ultrahle_match_ini(const std::string& title) {
  std::unordered_map<std::string, int> out;
  if (auto* p = ultrahle_find_profile(title)) {
    if (p->ismario) out["ismario"]=1;
    if (p->iszelda) out["iszelda"]=1;
    if (p->bootloader) out["bootloader"]=1;
  }
  return out;
}



struct OpName { int code; const char* name; bool reserved; };
static const OpName PRIMARY_OPS[] = {
  {0x00,"SPECIAL",0},{0x01,"REGIMM",0},{0x02,"J",0},{0x03,"JAL",0},{0x04,"BEQ",0},{0x05,"BNE",0},{0x06,"BLEZ",0},{0x07,"BGTZ",0},
  {0x08,"ADDI",0},{0x09,"ADDIU",0},{0x0A,"SLTI",0},{0x0B,"SLTIU",0},{0x0C,"ANDI",0},{0x0D,"ORI",0},{0x0E,"XORI",0},{0x0F,"LUI",0},
  {0x10,"COP0",0},{0x11,"COP1",0},{0x12,"COP2",0},{0x13,"COP3",0},{0x14,"BEQL",0},{0x15,"BNEL",0},{0x16,"BLEZL",0},{0x17,"BGTZL",0},
  {0x18,"DADDI",0},{0x19,"DADDIU",0},{0x1A,"LDL",0},{0x1B,"LDR",0},{0x1C,"PATCH",0},{0x1D,"GROUP",0},{0x1E,"RESERVED_1E",1},{0x1F,"RESERVED_1F",1},
  {0x20,"LB",0},{0x21,"LH",0},{0x22,"LWL",0},{0x23,"LW",0},{0x24,"LBU",0},{0x25,"LHU",0},{0x26,"LWR",0},{0x27,"LWU",0},
  {0x28,"SB",0},{0x29,"SH",0},{0x2A,"SWL",0},{0x2B,"SW",0},{0x2C,"SDL",0},{0x2D,"SDR",0},{0x2E,"SWR",0},{0x2F,"CACHE",0},
  {0x30,"LL",0},{0x31,"LWC1",0},{0x32,"LWC2",1},{0x33,"LWC3",1},{0x34,"LLD",0},{0x35,"LDC1",0},{0x36,"LDC2",1},{0x37,"LD",0},
  {0x38,"SC",0},{0x39,"SWC1",0},{0x3A,"SWC2",1},{0x3B,"SWC3",1},{0x3C,"SCD",0},{0x3D,"SDC1",0},{0x3E,"SDC2",1},{0x3F,"SD",0},
};
static const OpName SPECIAL_OPS[] = {
  {0x00,"SLL",0},{0x01,"RESERVED_SLL_01",1},{0x02,"SRL",0},{0x03,"SRA",0},{0x04,"SLLV",0},{0x05,"RESERVED_SLLV_05",1},{0x06,"SRLV",0},{0x07,"SRAV",0},
  {0x08,"JR",0},{0x09,"JALR",0},{0x0A,"MOVZ",0},{0x0B,"MOVN",0},{0x0C,"SYSCALL",0},{0x0D,"BREAK",0},{0x0E,"RESERVED_SP_0E",1},{0x0F,"SYNC",0},
  {0x10,"MFHI",0},{0x11,"MTHI",0},{0x12,"MFLO",0},{0x13,"MTLO",0},{0x14,"DSLLV",0},{0x15,"RESERVED_DSLLV_15",1},{0x16,"DSRLV",0},{0x17,"DSRAV",0},
  {0x18,"MULT",0},{0x19,"MULTU",0},{0x1A,"DIV",0},{0x1B,"DIVU",0},{0x1C,"DMULT",0},{0x1D,"DMULTU",0},{0x1E,"DDIV",0},{0x1F,"DDIVU",0},
  {0x20,"ADD",0},{0x21,"ADDU",0},{0x22,"SUB",0},{0x23,"SUBU",0},{0x24,"AND",0},{0x25,"OR",0},{0x26,"XOR",0},{0x27,"NOR",0},
  {0x28,"RESERVED_SP_28",1},{0x29,"RESERVED_SP_29",1},{0x2A,"SLT",0},{0x2B,"SLTU",0},{0x2C,"DADD",0},{0x2D,"DADDU",0},{0x2E,"DSUB",0},{0x2F,"DSUBU",0},
  {0x30,"TGE",0},{0x31,"TGEU",0},{0x32,"TLT",0},{0x33,"TLTU",0},{0x34,"TEQ",0},{0x35,"RESERVED_SP_35",1},{0x36,"TNE",0},{0x37,"RESERVED_SP_37",1},
  {0x38,"DSLL",0},{0x39,"RESERVED_DSLL_39",1},{0x3A,"DSRL",0},{0x3B,"DSRA",0},{0x3C,"DSLL32",0},{0x3D,"RESERVED_DSLL32_3D",1},{0x3E,"DSRL32",0},{0x3F,"DSRA32",0},
};
static const OpName REGIMM_OPS[] = {
  {0x00,"BLTZ",0},{0x01,"BGEZ",0},{0x02,"BLTZL",0},{0x03,"BGEZL",0},{0x04,"RESERVED_RI_04",1},{0x05,"RESERVED_RI_05",1},{0x06,"RESERVED_RI_06",1},{0x07,"RESERVED_RI_07",1},
  {0x08,"TGEI",0},{0x09,"TGEIU",0},{0x0A,"TLTI",0},{0x0B,"TLTIU",0},{0x0C,"TEQI",0},{0x0D,"RESERVED_RI_0D",1},{0x0E,"TNEI",0},{0x0F,"RESERVED_RI_0F",1},
  {0x10,"BLTZAL",0},{0x11,"BGEZAL",0},{0x12,"BLTZALL",0},{0x13,"BGEZALL",0},
};
static const OpName COP0_RS_OPS[] = {
  {0x00,"MFC0",0},{0x01,"DMFC0",0},{0x02,"CFC0",0},{0x04,"MTC0",0},{0x05,"DMTC0",0},{0x06,"CTC0",0},{0x08,"BC0",0},{0x10,"COP0_CO",0},
};
static const OpName COP0_CO_OPS[] = {
  {0x01,"TLBR",0},{0x02,"TLBWI",0},{0x06,"TLBWR",0},{0x08,"TLBP",0},{0x18,"ERET",0},{0x20,"WAIT",0},
};
static const OpName COP1_RS_OPS[] = {
  // Project64-Legacy Interpreter Ops.h COP1 surface (incl. DCF/DCT)
  {0x00,"MFC1",0},{0x01,"DMFC1",0},{0x02,"CFC1",0},{0x03,"DCFC1",0},{0x04,"MTC1",0},{0x05,"DMTC1",0},{0x06,"CTC1",0},{0x07,"DCTC1",0},{0x08,"BC1",0},
  {0x10,"S",0},{0x11,"D",0},{0x14,"W",0},{0x15,"L",0},
};
static const OpName COP2_RS_OPS[] = {
  {0x00,"MFC2",0},{0x01,"DMFC2",0},{0x02,"CFC2",0},{0x03,"DCFC2",0},{0x04,"MTC2",0},{0x05,"DMTC2",0},{0x06,"CTC2",0},{0x07,"DCTC2",0},
};
static const char* COP1_FUNCT_NAMES[] = {
  "ADD","SUB","MUL","DIV","SQRT","ABS","MOV","NEG","ROUND.L","TRUNC.L","CEIL.L","FLOOR.L",
  "ROUND.W","TRUNC.W","CEIL.W","FLOOR.W",nullptr,nullptr,nullptr,nullptr,nullptr,"RECIP","RSQRT",nullptr,nullptr,nullptr,nullptr,nullptr,nullptr,nullptr,nullptr,nullptr,
  "CVT.S","CVT.D",nullptr,nullptr,"CVT.W","CVT.L",nullptr,nullptr,nullptr,nullptr,nullptr,nullptr,nullptr,nullptr,nullptr,nullptr,
  "C.F","C.UN","C.EQ","C.UEQ","C.OLT","C.ULT","C.OLE","C.ULE","C.SF","C.NGLE","C.SEQ","C.NGL","C.LT","C.NGE","C.LE","C.NGT"
};
static const int COP1_FUNCT_CODES[] = {
  0x00,0x01,0x02,0x03,0x04,0x05,0x06,0x07,0x08,0x09,0x0A,0x0B,0x0C,0x0D,0x0E,0x0F,
  0x15,0x16,0x20,0x21,0x24,0x25,0x30,0x31,0x32,0x33,0x34,0x35,0x36,0x37,0x38,0x39,0x3A,0x3B,0x3C,0x3D,0x3E,0x3F
};

struct OsCallSig {
  u32 data[8]; u32 crc1; u32 crc2; int patch; const char* name;
};
static const OsCallSig ULTRAHLE_OSCALL[] = {
  {{0x40024002u,0x03E003E0u,0x00000000u,0x00000000u,0x00000000u,0x00000000u,0x00000000u,0x00000000u},0x00000000u,0x40008200u,1,"__osGetCause  #1"},
  {{0x40024002u,0x03E003E0u,0x00000000u,0x00000000u,0x00000000u,0x00000000u,0x00000000u,0x00000000u},0x00000000u,0x4000B200u,1,"__osGetCompare  #1"},
  {{0x40024002u,0x03E003E0u,0x00000000u,0x00000000u,0x00000000u,0x00000000u,0x00000000u,0x00000000u},0x00000000u,0x40006A00u,1,"__osGetConfig  #1"},
  {{0x40024002u,0x03E003E0u,0x00000000u,0x00000000u,0x00000000u,0x00000000u,0x00000000u,0x00000000u},0x00000000u,0x4000A200u,48,"osGetCount #48"},
  {{0x44424442u,0x03E003E0u,0x00000000u,0x00000000u,0x00000000u,0x00000000u,0x00000000u,0x00000000u},0x00000000u,0x44401200u,1,"__osGetFpcCsr  #1"},
  {{0x40024002u,0x03E003E0u,0x00000000u,0x00000000u,0x00000000u,0x00000000u,0x00000000u,0x00000000u},0x00000000u,0x40008A00u,1,"__osGetSR #1"},
  {{0x40024002u,0x30423042u,0x3C083C08u,0x25082508u,0x8D098D09u,0x24012401u,0x01210121u,0x31083108u},0x000028AAu,0x44E69EF4u,2,"osGetIntMask  #2"},
  {{0x40024002u,0x03E003E0u,0x00000000u,0x00000000u,0x00000000u,0x00000000u,0x00000000u,0x00000000u},0x00000000u,0x4000BA00u,1,"__osGetTLBASID  #1"},
  {{0x40844084u,0x00000000u,0x42004200u,0x00000000u,0x00000000u,0x00000000u,0x40024002u,0x03E003E0u},0x00000000u,0x4284D1C5u,1,"__osGetTLBHi  #1"},
  {{0x40844084u,0x00000000u,0x42004200u,0x00000000u,0x00000000u,0x00000000u,0x40024002u,0x03E003E0u},0x00000000u,0x428491C5u,1,"__osGetTLBLo0  #1"},
  {{0x40844084u,0x00000000u,0x42004200u,0x00000000u,0x00000000u,0x00000000u,0x40024002u,0x03E003E0u},0x00000000u,0x428499C5u,1,"__osGetTLBLo1  #1"},
  {{0x40844084u,0x00000000u,0x42004200u,0x00000000u,0x00000000u,0x00000000u,0x40024002u,0x03E003E0u},0x00000000u,0x4284A9C5u,1,"__osGetTLBPageMask  #1"},
  {{0x18A018A0u,0x00000000u,0x240B240Bu,0x00AB00ABu,0x10201020u,0x00000000u,0x00800080u,0x00850085u},0x00003A15u,0x86C122C7u,1,"osInvalDCache #1"},
  {{0x18A018A0u,0x00000000u,0x240B240Bu,0x00AB00ABu,0x10201020u,0x00000000u,0x00800080u,0x00850085u},0x00001A15u,0xFCC122C7u,38,"osInvalICache #38"},
  {{0x40084008u,0x24012401u,0x01010101u,0x40894089u,0x31023102u,0x00000000u,0x03E003E0u,0x00000000u},0x00000012u,0x0081B12Cu,1,"__osDisableInt #1"},
  {{0x40084008u,0x01040104u,0x40884088u,0x00000000u,0x00000000u,0x03E003E0u,0x00000000u,0x00000000u},0x00000000u,0x06843070u,1,"__osRestoreInt #1"},
  {{0x40084008u,0x40844084u,0x40854085u,0x8FA98FA9u,0x24012401u,0x11211121u,0x240C240Cu,0x240A240Au},0x000035F0u,0x5DB45780u,24,"osMapTLB #24"},
  {{0x400C400Cu,0x31823182u,0x3C083C08u,0x25082508u,0x8D0B8D0Bu,0x24012401u,0x01610161u,0x31083108u},0x0000A8AAu,0xAE358CFCu,2,"osSetIntMask #2"},
  {{0x40844084u,0x03E003E0u,0x00000000u,0x00000000u,0x00000000u,0x00000000u,0x00000000u,0x00000000u},0x00000000u,0x40868200u,1,"__osSetCause  #1"},
  {{0x40844084u,0x03E003E0u,0x00000000u,0x00000000u,0x00000000u,0x00000000u,0x00000000u,0x00000000u},0x00000000u,0x4086B200u,1,"__osSetCompare #1"},
  {{0x40844084u,0x03E003E0u,0x00000000u,0x00000000u,0x00000000u,0x00000000u,0x00000000u,0x00000000u},0x00000000u,0x40866A00u,1,"__osSetConfig  #1"},
  {{0x40844084u,0x03E003E0u,0x00000000u,0x00000000u,0x00000000u,0x00000000u,0x00000000u,0x00000000u},0x00000000u,0x4086A200u,1,"__osSetCount  #1"},
  {{0x44424442u,0x44C444C4u,0x03E003E0u,0x00000000u,0x00000000u,0x00000000u,0x00000000u,0x00000000u},0x00000000u,0x00863700u,1,"__osSetFpcCsr #1"},
  {{0x40844084u,0x00000000u,0x03E003E0u,0x00000000u,0x00000000u,0x00000000u,0x00000000u,0x00000000u},0x00000000u,0x40845710u,1,"__osSetSR  #1"},
  {{0x40844084u,0x03E003E0u,0x00000000u,0x00000000u,0x00000000u,0x00000000u,0x00000000u,0x00000000u},0x00000000u,0x4086BA00u,2,"osSetTLBASID  #2"},
  {{0x40084008u,0x40844084u,0x3C093C09u,0x40894089u,0x40804080u,0x40804080u,0x00000000u,0x42004200u},0x00000000u,0x5AC36802u,2,"osUnmapTLB  #2"},
  {{0x40084008u,0x24092409u,0x3C0A3C0Au,0x408A408Au,0x40804080u,0x40804080u,0x40894089u,0x00000000u},0x00000802u,0x484FD806u,2,"osUnmapTLBAll  #2"},
  {{0x18A018A0u,0x00000000u,0x240B240Bu,0x00AB00ABu,0x10201020u,0x00000000u,0x00800080u,0x00850085u},0x00001A15u,0xCCC122C7u,2,"osWritebackDCache #2"},
  {{0x3C083C08u,0x240A240Au,0x010A010Au,0x25292529u,0xBD01BD01u,0x01090109u,0x14201420u,0x25082508u},0x000000CAu,0x000E9F38u,1,"osWritebackDCacheAll #1"},
  {{0x40084008u,0x24092409u,0x40894089u,0x40804080u,0x240A240Au,0x3C093C09u,0x40894089u,0x3C093C09u},0x00000812u,0x4ACCA206u,2,"osMapTLBRdb #2"},
  {{0x27BD27BDu,0xAFBFAFBFu,0x00000000u,0xAFA4AFA4u,0x8FAE8FAEu,0xAFA2AFA2u,0x8DCF8DCFu,0x11E011E0u},0x00001A81u,0xEE2EC003u,1,"__osAtomicDec  #1"},
  {{0x3C0E3C0Eu,0x3C0F3C0Fu,0x25CE25CEu,0x25EF25EFu,0xAC8EAC8Eu,0xAC8FAC8Fu,0xAC80AC80u,0xAC80AC80u},0x0000000Cu,0x001D0124u,13,"osCreateMesgQueue #13"},
  {{0x27BD27BDu,0xAFA4AFA4u,0xAFA5AFA5u,0x8FAE8FAEu,0x8FAF8FAFu,0xAFBFAFBFu,0xAFA6AFA6u,0xAFA7AFA7u},0x00000001u,0x7B04BF73u,10,"osCreateThread #10"},
  {{0x3C1A3C1Au,0x275A275Au,0x03400340u,0x00000000u,0x3C1A3C1Au,0x275A275Au,0xFF41FF41u,0x401B401Bu},0x00000222u,0x9EA512E4u,1,"__osExceptionPreamble  #1"},
  {{0x3C1A3C1Au,0x275A275Au,0xFF41FF41u,0x401B401Bu,0xAF5BAF5Bu,0x24012401u,0x03610361u,0x409B409Bu},0x00008022u,0xFC437204u,1,"__osException  #1"},
  {{0x3C0A3C0Au,0x254A254Au,0x01440144u,0x8D498D49u,0x03E003E0u,0x11201120u,0x00000000u,0x8D2B8D2Bu},0x00008422u,0xE07AFA48u,0,"send_mesg"},
  {{0x3C013C01u,0x01010101u,0x00090009u,0x240A240Au,0x152A152Au,0x00000000u,0x8F5B8F5Bu,0x3C013C01u},0x00004918u,0x481AC224u,0,"handle_CpU"},
  {{0x3C053C05u,0x8CA58CA5u,0x40084008u,0x8CBB8CBBu,0x35083508u,0xACA8ACA8u,0xFCB0FCB0u,0xFCB1FCB1u},0x00000012u,0x73042C40u,1,"__osEnqueueAndYield  #1"},
  {{0x8C988C98u,0x8CAF8CAFu,0x00800080u,0x8F0E8F0Eu,0x01CF01CFu,0x14201420u,0x00000000u,0x03000300u},0x00000820u,0x97A7BB74u,1,"__osEnqueueThread  #1"},
  {{0x8C828C82u,0x8C598C59u,0x03E003E0u,0xAC99AC99u,0x00000000u,0x00000000u,0x00000000u,0x00000000u},0x00000000u,0x0000579Eu,1,"__osPopThread  #1"},
  {{0x3C043C04u,0x00000000u,0x24842484u,0x3C013C01u,0xAC22AC22u,0x24082408u,0xA448A448u,0x00400040u},0x0000E434u,0x08CBC6E4u,1,"__osDispatchThread  #1"},
  {{0x00000000u,0x00000000u,0x00000000u,0x00000000u,0x27BD27BDu,0xAFBFAFBFu,0xAFA4AFA4u,0xAFB2AFB2u},0x00002010u,0x12ECFAE7u,1,"__osCleanupThread  #1"},
  {{0x27BD27BDu,0xAFBFAFBFu,0xAFA4AFA4u,0xAFB2AFB2u,0xAFB1AFB1u,0x00000000u,0xAFB0AFB0u,0x8FAE8FAEu},0x00003201u,0x0E471BAFu,2,"osDestroyThread  #2"},
  {{0x3C023C02u,0x03E003E0u,0x8C428C42u,0x00000000u,0x00000000u,0x00000000u,0x00000000u,0x00000000u},0x00000004u,0x00CCEA0Fu,1,"__osGetActiveQueue  #1"},
  {{0x14801480u,0x00000000u,0x3C043C04u,0x8C848C84u,0x03E003E0u,0x8C828C82u,0x00000000u,0x00000000u},0x00000009u,0x00E1B01Du,2,"osGetThreadId  #2"},
  {{0x14801480u,0x00000000u,0x3C043C04u,0x8C848C84u,0x03E003E0u,0x8C828C82u,0x00000000u,0x00000000u},0x00000009u,0x00F1B01Du,2,"osGetThreadPri #2"},
  {{0x27BD27BDu,0xAFBFAFBFu,0x00000000u,0xAFB0AFB0u,0x00000000u,0x00400040u,0xAFA2AFA2u,0x3C0F3C0Fu},0x00003101u,0x126BB64Bu,19,"osGetTime #19"},
  {{0x27BD27BDu,0xAFBFAFBFu,0x240E240Eu,0x3C013C01u,0xAFB0AFB0u,0xAFA0AFA0u,0x00000000u,0xAC2EAC2Eu},0x00004085u,0x04AFB7CBu,2,"osInitialize #2"},
  {{0x27BD27BDu,0xAFBFAFBFu,0xAFA4AFA4u,0xAFA5AFA5u,0xAFA6AFA6u,0xAFB1AFB1u,0x00000000u,0xAFB0AFB0u},0x00002001u,0xD019FDBBu,15,"osJamMesg #15"},
  {{0x3C013C01u,0x03E003E0u,0x00810081u,0x00000000u,0x00000000u,0x00000000u,0x00000000u,0x00000000u},0x00000000u,0x00B6EA0Fu,29,"osPhysicalToVirtual #29"},
  {{0x27BD27BDu,0xAFBFAFBFu,0xAFA4AFA4u,0xAFA5AFA5u,0xAFA6AFA6u,0xAFB1AFB1u,0x00000000u,0xAFB0AFB0u},0x00004801u,0x2359FDBBu,14,"osRecvMesg #14"},
  {{0x27BD27BDu,0xAFBFAFBFu,0xAFA4AFA4u,0x00000000u,0xAFB0AFB0u,0x8FAF8FAFu,0x3C0E3C0Eu,0x8DCE8DCEu},0x00004181u,0xE0281BFBu,1,"__osResetGlobalIntMask  #1"},
  {{0x27BD27BDu,0xAFBFAFBFu,0xAFA4AFA4u,0xAFA5AFA5u,0xAFA6AFA6u,0xAFB2AFB2u,0xAFB1AFB1u,0x00000000u},0x00004001u,0x361417EFu,15,"osSendMesg #15"},
  {{0x27BD27BDu,0xAFBFAFBFu,0xAFA4AFA4u,0xAFA5AFA5u,0xAFA6AFA6u,0x00000000u,0xAFB0AFB0u,0x8FAE8FAEu},0x00000401u,0x1AFB59BBu,16,"osSetEventMesg #16"},
  {{0x27BD27BDu,0xAFBFAFBFu,0xAFA4AFA4u,0x00000000u,0xAFB0AFB0u,0x3C0E3C0Eu,0x8DCE8DCEu,0x8FAF8FAFu},0x00000841u,0x00FF13FBu,1,"__osSetGlobalIntMask  #1"},
  {{0x27BD27BDu,0xAFBFAFBFu,0xAFA4AFA4u,0xAFA5AFA5u,0x00000000u,0xAFB0AFB0u,0x8FAF8FAFu,0x8FAE8FAEu},0x00004001u,0x555B1ABBu,1,"__osSetHWIntrRoutine  #1"},
  {{0x27BD27BDu,0xAFBFAFBFu,0xAFA4AFA4u,0xAFA5AFA5u,0x00000000u,0xAFB0AFB0u,0x8FAE8FAEu,0x00400040u},0x00000901u,0x4F2CCABBu,18,"osSetThreadPri #18"},
  {{0xAFA4AFA4u,0x8FAE8FAEu,0xAFA5AFA5u,0x3C013C01u,0x8FAF8FAFu,0xAC2EAC2Eu,0x3C013C01u,0x03E003E0u},0x00000120u,0x000392F9u,2,"osSetTime #2"},
  {{0x27BD27BDu,0xAFA4AFA4u,0x8FAE8FAEu,0xAFBFAFBFu,0xAFA6AFA6u,0xAFA7AFA7u,0xADC0ADC0u,0x8FAF8FAFu},0x00000001u,0xB4E4AEBBu,33,"osSetTimer #33"},
  {{0x27BD27BDu,0xAFBFAFBFu,0xAFA4AFA4u,0xAFB1AFB1u,0x00000000u,0xAFB0AFB0u,0x8FAE8FAEu,0x24012401u},0x00009C81u,0xCACB063Bu,11,"osStartThread #11"},
  {{0x27BD27BDu,0xAFBFAFBFu,0xAFA4AFA4u,0xAFB2AFB2u,0xAFB1AFB1u,0x00000000u,0xAFB0AFB0u,0x8FAE8FAEu},0x00001A01u,0x64071BAFu,28,"osStopThread #28"},
  {{0x27BD27BDu,0xAFA4AFA4u,0x8FAE8FAEu,0xAFBFAFBFu,0xAFB0AFB0u,0x8DCF8DCFu,0x15E015E0u,0x00000000u},0x00004341u,0x396BBF2Fu,34,"osStopTimer #34"},
  {{0x00800080u,0x8CC78CC7u,0x27BD27BDu,0x10E010E0u,0x00000000u,0x14E514E5u,0x00000000u,0x8CAE8CAEu},0x0000912Cu,0xDA4BB5D7u,1,"__osDequeueThread  #1"},
  {{0x3C013C01u,0x240E240Eu,0x240F240Fu,0xAC2FAC2Fu,0xAC2EAC2Eu,0x3C013C01u,0x3C183C18u,0x8F188F18u},0x0000A59Eu,0xF15FD19Cu,1,"__osTimerServicesInit  #1"},
  {{0x3C0E3C0Eu,0x8DCE8DCEu,0x27BD27BDu,0xAFBFAFBFu,0x8DCF8DCFu,0x11EE11EEu,0x00000000u,0x3C183C18u},0x00008526u,0x782FB29Cu,1,"__osTimerInterrupt  #1"},
  {{0x27BD27BDu,0xAFBFAFBFu,0xAFA4AFA4u,0x00000000u,0xAFA5AFA5u,0x00000000u,0xAFA2AFA2u,0x3C013C01u},0x00008501u,0x2F6DB8F3u,1,"__osSetTimerIntr  #1"},
  {{0x27BD27BDu,0xAFBFAFBFu,0x00000000u,0xAFA4AFA4u,0x3C0E3C0Eu,0x8DCE8DCEu,0xAFA2AFA2u,0x8FB88FB8u},0x00002021u,0x3915AAC3u,1,"__osInsertTimer  #1"},
  {{0x27BD27BDu,0xAFA4AFA4u,0x8FAE8FAEu,0x3C013C01u,0xAFBFAFBFu,0x01C101C1u,0x14201420u,0x3C013C01u},0x00001A41u,0x0E2098EFu,21,"osVirtualToPhysical #21"},
  {{0x40084008u,0x31093109u,0x24012401u,0x00810081u,0x012A012Au,0x40894089u,0x00000000u,0x00000000u},0x00008006u,0xBAC8B3C8u,1,"__osProbeTLB #1"},
  {{0x27BD27BDu,0xAFBFAFBFu,0x00000000u,0xAFB0AFB0u,0x3C0F3C0Fu,0x8DEF8DEFu,0x240E240Eu,0x3C043C04u},0x00000261u,0x002F964Bu,2,"osYieldThread  #2"},
  {{0x3C023C02u,0x03E003E0u,0x8C428C42u,0x00000000u,0x00000000u,0x00000000u,0x00000000u,0x00000000u},0x00000004u,0x00CCEA0Fu,1,"__osGetCurrFaultedThread  #1"},
  {{0x27BD27BDu,0xAFBFAFBFu,0xAFA4AFA4u,0xAFB1AFB1u,0x00000000u,0xAFB0AFB0u,0x8FAE8FAEu,0x00400040u},0x00009901u,0x922CC63Bu,1,"__osGetNextFaultedThread  #1"},
  {{0x3C0E3C0Eu,0x03E003E0u,0x8DC28DC2u,0x00000000u,0x00000000u,0x00000000u,0x00000000u,0x00000000u},0x00000000u,0x3C474E50u,31,"osAiGetLength #31"},
  {{0x3C0E3C0Eu,0x03E003E0u,0x8DC28DC2u,0x00000000u,0x00000000u,0x00000000u,0x00000000u,0x00000000u},0x00000000u,0x3C4F4E50u,2,"osAiGetStatus  #2"},
  {{0x3C0E3C0Eu,0x8DCE8DCEu,0x44844484u,0x27BD27BDu,0x448E448Eu,0x46804680u,0x04810481u,0x46804680u},0x0000800Au,0xC813D50Fu,32,"osAiSetFrequency #32"},
  {{0x27BD27BDu,0x3C0F3C0Fu,0x91EF91EFu,0xAFA4AFA4u,0x8FAE8FAEu,0xAFBFAFBFu,0xAFA5AFA5u,0x11E011E0u},0x0000A285u,0x20D5C417u,30,"osAiSetNextBuffer #30"},
  {{0x3C0E3C0Eu,0x8DC48DC4u,0x3C013C01u,0x27BD27BDu,0x00810081u,0x11E011E0u,0x00000000u,0x10001000u},0x000009A8u,0x3C8D5150u,1,"__osAiDeviceBusy #1"},
  {{0x3C0E3C0Eu,0x03E003E0u,0x8DC28DC2u,0x00000000u,0x00000000u,0x00000000u,0x00000000u,0x00000000u},0x00000000u,0x3C4F4E10u,2,"osDpGetStatus  #2"},
  {{0x3C0E3C0Eu,0x03E003E0u,0xADC4ADC4u,0x00000000u,0x00000000u,0x00000000u,0x00000000u,0x00000000u},0x00000000u,0x3C694E10u,2,"osDpSetStatus  #2"},
  {{0x27BD27BDu,0xAFBFAFBFu,0xAFA4AFA4u,0xAFA6AFA6u,0xAFA7AFA7u,0x00000000u,0xAFB0AFB0u,0x10401040u},0x00000E81u,0x6002E21Bu,2,"osDpSetNextBuffer  #2"},
  {{0x3C0E3C0Eu,0x8DC48DC4u,0x27BD27BDu,0x308F308Fu,0x11E011E0u,0x00000000u,0x10001000u,0x24022402u},0x000004DCu,0x3C334480u,1,"__osDpDeviceBusy  #1"},
  {{0x3C0E3C0Eu,0x8DCF8DCFu,0x3C183C18u,0x3C083C08u,0xAC8FAC8Fu,0x8F198F19u,0x24842484u,0x3C0A3C0Au},0x00004C40u,0xE27700ECu,2,"osDpGetCounters  #2"},
  {{0x3C0E3C0Eu,0x03E003E0u,0x8DC28DC2u,0x00000000u,0x00000000u,0x00000000u,0x00000000u,0x00000000u},0x00000000u,0x3C534E04u,1,"__osSpGetStatus #1"},
  {{0x3C0E3C0Eu,0x03E003E0u,0xADC4ADC4u,0x00000000u,0x00000000u,0x00000000u,0x00000000u,0x00000000u},0x00000000u,0x3C754E04u,1,"__osSpSetStatus #1"},
  {{0x3C0E3C0Eu,0x8DC58DC5u,0x27BD27BDu,0x30AF30AFu,0x15E015E0u,0x00000000u,0x10001000u,0x24022402u},0x000010DCu,0x785CE920u,1,"__osSpSetPc #1"},
  {{0x27BD27BDu,0xAFBFAFBFu,0xAFA4AFA4u,0x00000000u,0xAFA5AFA5u,0x10401040u,0x00000000u,0x10001000u},0x000001A1u,0x62EFF3C3u,1,"__osSpRawReadIo  #1"},
  {{0x3C0E3C0Eu,0x8DC48DC4u,0x27BD27BDu,0x308F308Fu,0x11E011E0u,0x00000000u,0x10001000u,0x24022402u},0x000004DCu,0x3C33449Cu,1,"__osSpDeviceBusy #1"},
  {{0x27BD27BDu,0xAFBFAFBFu,0xAFA4AFA4u,0x00000000u,0xAFA5AFA5u,0x10401040u,0x00000000u,0x10001000u},0x000001A1u,0xC75BF3C3u,1,"__osSpRawWriteIo  #1"},
  {{0x27BD27BDu,0xAFBFAFBFu,0xAFA4AFA4u,0xAFA5AFA5u,0xAFA6AFA6u,0x00000000u,0xAFA7AFA7u,0x10401040u},0x00000681u,0x7F2A4C47u,1,"__osSpRawStartDma #1"},
  {{0x27BD27BDu,0xAFBFAFBFu,0xAFA4AFA4u,0x00000000u,0x8FA48FA4u,0xAFA2AFA2u,0x8FAE8FAEu,0x8DCF8DCFu},0x00001301u,0xF94620D3u,39,"osSpTaskLoad #39"},
  {{0x27BD27BDu,0xAFBFAFBFu,0x00000000u,0xAFA4AFA4u,0x10401040u,0x00000000u,0x00000000u,0x00000000u},0x00002911u,0x14EFB143u,22,"osSpTaskStartGo #22"},
  {{0x27BD27BDu,0xAFBFAFBFu,0x00000000u,0x24042404u,0x8FBF8FBFu,0x27BD27BDu,0x03E003E0u,0x00000000u},0x00000029u,0x0000BE43u,46,"osSpTaskYield #46"},
  {{0x27BD27BDu,0xAFBFAFBFu,0x00000000u,0xAFA4AFA4u,0xAFA2AFA2u,0x8FAE8FAEu,0x31CF31CFu,0x11E011E0u},0x0000C6C1u,0x105EE503u,47,"osSpTaskYielded #47"},
  {{0x27BD27BDu,0xAFBFAFBFu,0x3C043C04u,0x24842484u,0x00000000u,0x24052405u,0x3C0E3C0Eu,0x25CE25CEu},0x00003AA9u,0x0A2F9EC3u,1,"__osViInit  #1"},
  {{0x3C0E3C0Eu,0x8DC28DC2u,0x304F304Fu,0x03E003E0u,0x01E001E0u,0x00000000u,0x00000000u,0x00000000u},0x00000004u,0x3C03CEF0u,2,"osViGetCurrentField #2"},
  {{0x27BD27BDu,0xAFBFAFBFu,0x00000000u,0xAFB0AFB0u,0x3C0E3C0Eu,0x8DCE8DCEu,0x00400040u,0x02000200u},0x00008021u,0xD92C2A4Bu,20,"osViGetCurrentFramebuffer #20"},
  {{0x27BD27BDu,0xAFBFAFBFu,0x00000000u,0xAFB0AFB0u,0x3C0E3C0Eu,0x8DCE8DCEu,0x00400040u,0x02000200u},0x00008021u,0xD92C2A4Bu,20,"osViGetNextFramebuffer #20"},
  {{0x3C0E3C0Eu,0x03E003E0u,0x8DC28DC2u,0x00000000u,0x00000000u,0x00000000u,0x00000000u,0x00000000u},0x00000000u,0x3C534E40u,2,"osViGetCurrentLine #2"},
  {{0x27BD27BDu,0xAFBFAFBFu,0xAFB1AFB1u,0x00000000u,0xAFB0AFB0u,0x3C0E3C0Eu,0x8DCE8DCEu,0x00400040u},0x00000041u,0xC6A8D0DBu,2,"osViGetCurrentMode  #2"},
  {{0x3C0E3C0Eu,0x03E003E0u,0x8DC28DC2u,0x00000000u,0x00000000u,0x00000000u,0x00000000u,0x00000000u},0x00000000u,0x3C434E40u,2,"osViGetStatus #2"},
  {{0x3C0E3C0Eu,0x8DCE8DCEu,0x27BD27BDu,0xAFBFAFBFu,0x15C015C0u,0xAFA4AFA4u,0x00000000u,0x00000000u},0x0000AC16u,0xF87FBB1Cu,55,"osCreateViManager #55"},
  {{0x3C023C02u,0x03E003E0u,0x8C428C42u,0x00000000u,0x00000000u,0x00000000u,0x00000000u,0x00000000u},0x00000004u,0x00CCEA0Fu,1,"__osViGetCurrentContext  #1"},
  {{0x27BD27BDu,0xAFBFAFBFu,0xAFA4AFA4u,0xAFA5AFA5u,0xAFA6AFA6u,0x00000000u,0xAFB0AFB0u,0x3C0F3C0Fu},0x00002101u,0x506399BBu,17,"osViSetEvent #17"},
  {{0x27BD27BDu,0xAFBFAFBFu,0xAFA4AFA4u,0x00000000u,0xAFB0AFB0u,0x3C0F3C0Fu,0x8DEF8DEFu,0x8FAE8FAEu},0x00008A41u,0xACCF63FBu,2,"osViSetMode #2"},
  {{0x27BD27BDu,0xAFBFAFBFu,0xAFA4AFA4u,0x00000000u,0xAFB0AFB0u,0x8FAE8FAEu,0x00400040u,0x31CF31CFu},0x00002981u,0x4C2FEFFBu,2,"osViSetSpecialFeatures #2"},
  {{0x27BD27BDu,0xAFBFAFBFu,0xE7ACE7ACu,0xAFB1AFB1u,0x00000000u,0xAFB0AFB0u,0x3C0E3C0Eu,0x8DCE8DCEu},0x00009081u,0xC66E162Bu,2,"osViSetXScale  #2"},
  {{0x27BD27BDu,0xAFBFAFBFu,0xE7ACE7ACu,0x00000000u,0xAFB0AFB0u,0x3C0E3C0Eu,0x8DCE8DCEu,0xC7A4C7A4u},0x00004841u,0xED0B93EBu,2,"osViSetYScale  #2"},
  {{0x27BD27BDu,0xAFBFAFBFu,0x00000000u,0xAFA4AFA4u,0x3C0F3C0Fu,0x8DEF8DEFu,0x8FAE8FAEu,0xAFA2AFA2u},0x00001421u,0x4A93A603u,23,"osViSwapBuffer #23"},
  {{0x27BD27BDu,0xAFB1AFB1u,0xAFBFAFBFu,0xAFB2AFB2u,0xAFB0AFB0u,0xAFA0AFA0u,0x3C113C11u,0x3C0E3C0Eu},0x00000901u,0xFB6943CFu,1,"__osViSwapContext  #1"},
  {{0x27BD27BDu,0xAFBFAFBFu,0xAFA4AFA4u,0x00000000u,0xAFB0AFB0u,0x93AE93AEu,0x00400040u,0x11C011C0u},0x00003481u,0x2C1E6BFBu,2,"osViBlack #2"},
  {{0x27BD27BDu,0xAFBFAFBFu,0xAFA4AFA4u,0xAFA5AFA5u,0x00000000u,0xAFB0AFB0u,0x93AE93AEu,0x00400040u},0x00003101u,0xF96DDABBu,2,"osViFade  #2"},
  {{0x3C0E3C0Eu,0x03E003E0u,0x8DC28DC2u,0x00000000u,0x00000000u,0x00000000u,0x00000000u,0x00000000u},0x00000000u,0x3C5B4E80u,1,"__osSiGetStatus  #1"},
  {{0x27BD27BDu,0xAFBFAFBFu,0xAFA4AFA4u,0x00000000u,0xAFA5AFA5u,0x10401040u,0x00000000u,0x10001000u},0x000001A1u,0x62EFF3C3u,1,"__osSiRawReadIo #1"},
  {{0x3C0E3C0Eu,0x8DC48DC4u,0x27BD27BDu,0x308F308Fu,0x11E011E0u,0x00000000u,0x10001000u,0x24022402u},0x000004DCu,0x3C334410u,1,"__osSiDeviceBusy #1"},
  {{0x27BD27BDu,0xAFBFAFBFu,0xAFA4AFA4u,0x00000000u,0xAFA5AFA5u,0x10401040u,0x00000000u,0x10001000u},0x000001A1u,0xC75BF3C3u,1,"__osSiRawWriteIo #1"},
  {{0x27BD27BDu,0xAFBFAFBFu,0xAFA4AFA4u,0x00000000u,0xAFA5AFA5u,0x10401040u,0x00000000u,0x10001000u},0x00008DA1u,0xCFAFF3C3u,1,"__osSiRawStartDma #1"},
  {{0x27BD27BDu,0xAFBFAFBFu,0xAFA4AFA4u,0x00000000u,0xAFA0AFA0u,0x3C0E3C0Eu,0x91CE91CEu,0x11C011C0u},0x000050C1u,0x344E73D3u,42,"osContStartQuery #42"},
  {{0x27BD27BDu,0xAFBFAFBFu,0xAFA4AFA4u,0x8FA58FA5u,0x00000000u,0x27A427A4u,0x8FBF8FBFu,0x27BD27BDu},0x000000A1u,0x000E46D3u,43,"osContGetQuery #43"},
  {{0x27BD27BDu,0xAFBFAFBFu,0x240E240Eu,0x3C013C01u,0x3C043C04u,0x3C053C05u,0xAC2EAC2Eu,0x24A524A5u},0x000015C5u,0x434E72C3u,1,"__osSiCreateAccessQueue #1"},
  {{0x3C0E3C0Eu,0x8DCE8DCEu,0x27BD27BDu,0xAFBFAFBFu,0x15C015C0u,0x00000000u,0x00000000u,0x00000000u},0x00005616u,0x274FB39Cu,1,"__osSiGetAccess #1"},
  {{0x27BD27BDu,0xAFBFAFBFu,0x3C043C04u,0x24842484u,0x00000000u,0x00000000u,0x00000000u,0x8FBF8FBFu},0x00000109u,0x000D6BC3u,1,"__osSiRelAccess #1"},
  {{0x27BD27BDu,0xAFBFAFBFu,0xAFA4AFA4u,0x00000000u,0xAFA0AFA0u,0x3C0E3C0Eu,0x91CE91CEu,0x24012401u},0x0000A1C1u,0x846C33D3u,26,"osContStartReadData #26"},
  {{0x3C0F3C0Fu,0x91EF91EFu,0x3C0E3C0Eu,0x27BD27BDu,0x25CE25CEu,0xAFAEAFAEu,0x19E019E0u,0xAFA0AFA0u},0x0000025Au,0x86C18908u,27,"osContGetReadData #27"},
  {{0x27BD27BDu,0xAFBFAFBFu,0xAFA4AFA4u,0x00000000u,0xAFA5AFA5u,0x3C0E3C0Eu,0x91CE91CEu,0x24012401u},0x0000A1C1u,0x846C32D3u,2,"osContReset  #2"},
  {{0x27BD27BDu,0xAFA0AFA0u,0x8FAE8FAEu,0x8FB88FB8u,0x3C013C01u,0x000E000Eu,0x002F002Fu,0xAC20AC20u},0x0000A781u,0xFCECCE97u,1,"__osPackResetData  #1"},
  {{0x27BD27BDu,0x3C0E3C0Eu,0x8DCE8DCEu,0xAFBFAFBFu,0xAFA4AFA4u,0xAFA5AFA5u,0xAFA6AFA6u,0x11C011C0u},0x00004A85u,0x04BA4E47u,25,"osContInit #25"},
  {{0x3C0F3C0Fu,0x91EF91EFu,0x27BD27BDu,0x3C0E3C0Eu,0x25CE25CEu,0xA3A0A3A0u,0xAFAEAFAEu,0x19E019E0u},0x00000496u,0x6D7C65D8u,1,"__osContGetInitData  #1"},
  {{0x27BD27BDu,0x30843084u,0xAFA0AFA0u,0x8FAE8FAEu,0x8FB88FB8u,0x3C013C01u,0x000E000Eu,0x002F002Fu},0x00004F03u,0x19C74027u,1,"__osPackRequestData  #1"},
  {{0x27BD27BDu,0xAFBFAFBFu,0xAFA4AFA4u,0xAFA5AFA5u,0x00000000u,0xAFA6AFA6u,0x8FA48FA4u,0x00000000u},0x00002801u,0xCF9F02EBu,2,"osPfsReFormat  #2"},
  {{0x27BD27BDu,0x8FB88FB8u,0x3C0E3C0Eu,0x25CE25CEu,0x240F240Fu,0x24012401u,0xAFBFAFBFu,0xAFA4AFA4u},0x00004039u,0x28E40ACBu,1,"__osContRamWrite  #1"},
  {{0x27BD27BDu,0x30843084u,0xA3A0A3A0u,0xAFA0AFA0u,0x93AE93AEu,0x31CF31CFu,0x11E011E0u,0x00000000u},0x0000A363u,0xB1DAA457u,1,"__osContAddressCrc  #1"},
  {{0x27BD27BDu,0xA3A0A3A0u,0xAFA0AFA0u,0x240E240Eu,0xAFAEAFAEu,0x93AF93AFu,0x31F831F8u,0x13001300u},0x000086C9u,0x915A314Bu,1,"__osContDataCrc  #1"},
  {{0x27BD27BDu,0xAFA0AFA0u,0xAFA4AFA4u,0x18A018A0u,0xAFA0AFA0u,0x8FAF8FAFu,0x8FAE8FAEu,0x8FAB8FABu},0x00000609u,0x27AAAE2Fu,1,"__osSumcalc  #1"},
  {{0x27BD27BDu,0xA7A0A7A0u,0xA4C0A4C0u,0x94CE94CEu,0xA4AEA4AEu,0xAFA0AFA0u,0x8FAF8FAFu,0x008F008Fu},0x00000001u,0x458111C3u,1,"__osIdCheckSum  #1"},
  {{0x27BD27BDu,0xAFA4AFA4u,0x8FAE8FAEu,0xAFBFAFBFu,0xAFA5AFA5u,0xAFA6AFA6u,0xAFA0AFA0u,0xA3A0A3A0u},0x00000201u,0x84084413u,1,"__osRepairPackId  #1"},
  {{0x27BD27BDu,0xAFA4AFA4u,0x8FAE8FAEu,0xAFBFAFBFu,0xAFA5AFA5u,0xAFA0AFA0u,0x91CF91CFu,0x11E011E0u},0x00004081u,0x38EAD507u,1,"__osCheckPackId  #1"},
  {{0x27BD27BDu,0xAFA4AFA4u,0x8FAE8FAEu,0xAFBFAFBFu,0x91CF91CFu,0x11E011E0u,0x00000000u,0xA1C0A1C0u},0x00005021u,0x0985F787u,1,"__osGetId  #1"},
  {{0x27BD27BDu,0xAFA4AFA4u,0x8FAE8FAEu,0xAFBFAFBFu,0x91CF91CFu,0x11E011E0u,0x00000000u,0xA1C0A1C0u},0x00005021u,0x09D5F787u,1,"__osCheckId  #1"},
  {{0x27BD27BDu,0xAFA4AFA4u,0x8FAE8FAEu,0xAFBFAFBFu,0xAFA5AFA5u,0xAFA6AFA6u,0xAFA7AFA7u,0x91CF91CFu},0x00008101u,0x955F1907u,1,"__osPfsRWInode  #1"},
  {{0x27BD27BDu,0xAFBFAFBFu,0xAFA4AFA4u,0xAFA0AFA0u,0xAFA0AFA0u,0x8FAE8FAEu,0x8FB88FB8u,0x91CF91CFu},0x00003801u,0x5138124Bu,1,"__osPfsSelectBank  #1"},
  {{0x27BD27BDu,0x3C0E3C0Eu,0xAFBFAFBFu,0x25CE25CEu,0x240F240Fu,0xAFA4AFA4u,0xAFA5AFA5u,0xAFA6AFA6u},0x00004019u,0x18741AD7u,1,"__osContRamRead  #1"},
  {{0x27BD27BDu,0xAFBFAFBFu,0xAFA4AFA4u,0xAFA0AFA0u,0x00000000u,0x8FA48FA4u,0xAFA2AFA2u,0x8FAE8FAEu},0x00008301u,0x4D40AA6Bu,2,"osPfsChecker  #2"},
  {{0x27BD27BDu,0xAFBFAFBFu,0xAFA4AFA4u,0xAFA5AFA5u,0xAFA0AFA0u,0x8FAE8FAEu,0x8FAF8FAFu,0x01CF01CFu},0x00009C01u,0x014D1113u,0,"corrupted_init"},
  {{0x27BD27BDu,0xAFA5AFA5u,0x93B893B8u,0x93AE93AEu,0xAFA4AFA4u,0x8FAA8FAAu,0xAFBFAFBFu,0xAFA6AFA6u},0x00009001u,0xBB51277Fu,0,"corrupted"},
  {{0x27BD27BDu,0xAFBFAFBFu,0xAFA4AFA4u,0xAFA5AFA5u,0xAFA6AFA6u,0x00000000u,0xAFA0AFA0u,0x8FA48FA4u},0x0000A001u,0x5665DA13u,2,"osPfsInit  #2"},
  {{0x27BD27BDu,0xAFBFAFBFu,0xAFA4AFA4u,0xAFA5AFA5u,0xAFA0AFA0u,0x00000000u,0x00000000u,0x3C053C05u},0x0000A501u,0x096DBBE3u,1,"__osPfsGetStatus  #1"},
  {{0x27BD27BDu,0xAFA5AFA5u,0x97AE97AEu,0xAFBFAFBFu,0xAFA4AFA4u,0xAFA6AFA6u,0xAFA7AFA7u,0xAFA0AFA0u},0x0000A401u,0x59FF4E7Bu,2,"osPfsAllocateFile  #2"},
  {{0x27BD27BDu,0x93AE93AEu,0xAFBFAFBFu,0xAFA4AFA4u,0xAFA5AFA5u,0xAFA6AFA6u,0xAFA7AFA7u,0x19C019C0u},0x00000681u,0xC690E4B3u,1,"__osPfsDeclearPage  #1"},
  {{0x27BD27BDu,0xAFA5AFA5u,0x97AE97AEu,0xAFBFAFBFu,0xAFA4AFA4u,0xAFA6AFA6u,0xAFA7AFA7u,0x11C011C0u},0x00003481u,0x2E454E7Bu,2,"osPfsDeleteFile  #2"},
  {{0x27BD27BDu,0xAFA6AFA6u,0x93B893B8u,0xAFA5AFA5u,0x8FAF8FAFu,0x00180018u,0xAFBFAFBFu,0xAFA4AFA4u},0x00009001u,0x82C6EE97u,1,"__osPfsReleasePages  #1"},
  {{0x27BD27BDu,0xAFA4AFA4u,0xAFA7AFA7u,0x93AE93AEu,0x8FAF8FAFu,0xAFBFAFBFu,0xAFA5AFA5u,0xAFA6AFA6u},0x00004001u,0x38E7B6E7u,1,"__osBlockSum  #1"},
  {{0x27BD27BDu,0xAFA4AFA4u,0x8FAF8FAFu,0xAFA5AFA5u,0xAFBFAFBFu,0xAFA6AFA6u,0xAFA7AFA7u,0x8FAE8FAEu},0x0000C401u,0x4A78E67Fu,2,"osPfsReadWriteFile  #2"},
  {{0x27BD27BDu,0xAFA4AFA4u,0x8FAF8FAFu,0xAFA5AFA5u,0xAFBFAFBFu,0xAFA6AFA6u,0x8FAE8FAEu,0x8DF88DF8u},0x00006201u,0x4F0300D7u,2,"osPfsFileState  #2"},
  {{0x27BD27BDu,0xAFBFAFBFu,0xAFA4AFA4u,0xAFA5AFA5u,0xAFA6AFA6u,0xAFA7AFA7u,0xAFA0AFA0u,0x00000000u},0x00003601u,0x1E461E43u,2,"osPfsFindFile  #2"},
  {{0x27BD27BDu,0xAFBFAFBFu,0xAFA4AFA4u,0xAFA5AFA5u,0x00000000u,0x8FA48FA4u,0x24012401u,0x14411441u},0x000016C1u,0xE8DA9EBBu,2,"osPfsSetLabel  #2"},
  {{0x27BD27BDu,0xAFA5AFA5u,0x8FAE8FAEu,0xAFBFAFBFu,0xAFA4AFA4u,0x15C015C0u,0xAFA6AFA6u,0x10001000u},0x0000D9A1u,0x4BDFCAABu,2,"osPfsGetLabel  #2"},
  {{0x27BD27BDu,0xAFBFAFBFu,0x240E240Eu,0xAFA4AFA4u,0xAFA5AFA5u,0xAFA0AFA0u,0xA3A0A3A0u,0x00000000u},0x00005005u,0xB44C11C3u,2,"osPfsIsPlug  #2"},
  {{0x27BD27BDu,0x30843084u,0x3C013C01u,0xA024A024u,0xAFA0AFA0u,0x8FAE8FAEu,0x8FB88FB8u,0x3C013C01u},0x00003C0Bu,0x33D1AD07u,1,"__osPfsRequestData  #1"},
  {{0x3C0F3C0Fu,0x91EF91EFu,0x27BD27BDu,0x3C0E3C0Eu,0x25CE25CEu,0xA3A0A3A0u,0xAFAEAFAEu,0x19E019E0u},0x00000496u,0x6D7C65D8u,1,"__osPfsGetInitData  #1"},
  {{0x27BD27BDu,0xAFA4AFA4u,0x8FAE8FAEu,0xAFBFAFBFu,0xAFA5AFA5u,0xAFA0AFA0u,0xAFA0AFA0u,0x8DCF8DCFu},0x00009B01u,0x860246BBu,2,"osPfsFreeBlocks  #2"},
  {{0x27BD27BDu,0xAFA4AFA4u,0x8FAE8FAEu,0xAFBFAFBFu,0xAFA5AFA5u,0xAFA6AFA6u,0xAFA0AFA0u,0x8DCF8DCFu},0x00009B01u,0x96025FAFu,2,"osPfsNumFiles  #2"},
  {{0x27BD27BDu,0xAFA5AFA5u,0x97AE97AEu,0xAFBFAFBFu,0xAFA4AFA4u,0xAFA6AFA6u,0xAFA7AFA7u,0xAFA0AFA0u},0x00004001u,0xE5354E7Bu,2,"osPfsReSizeFile  #2"},
  {{0x27BD27BDu,0xAFA5AFA5u,0x93AF93AFu,0x3C0E3C0Eu,0x25CE25CEu,0x29E129E1u,0xAFBFAFBFu,0xAFA4AFA4u},0x00006831u,0xCFF4A5D7u,58,"osEepromRead #58"},
  {{0x27BD27BDu,0xAFA5AFA5u,0x93AF93AFu,0x3C0E3C0Eu,0x25CE25CEu,0x29E129E1u,0xAFBFAFBFu,0xAFA4AFA4u},0x00003431u,0x0E64A5D7u,59,"osEepromWrite #59"},
  {{0x27BD27BDu,0x3C0E3C0Eu,0x25CE25CEu,0xAFBFAFBFu,0xAFA4AFA4u,0xAFA5AFA5u,0xAFA0AFA0u,0xAFAEAFAEu},0x0000C005u,0x0A462BA7u,1,"__osEepStatus #1"},
  {{0x27BD27BDu,0xAFBFAFBFu,0xAFA4AFA4u,0x00000000u,0xAFA0AFA0u,0x8FA48FA4u,0x00000000u,0x27A527A5u},0x00006481u,0x396AB7D3u,57,"osEepromProbe #57"},
  {{0x27BD27BDu,0xAFA5AFA5u,0x93AE93AEu,0xAFBFAFBFu,0xAFA4AFA4u,0x29C129C1u,0xAFA6AFA6u,0xAFA7AFA7u},0x00002D21u,0x971CBD47u,61,"osEepromLongWrite #61"},
  {{0x27BD27BDu,0xAFA5AFA5u,0x93AE93AEu,0xAFBFAFBFu,0xAFA4AFA4u,0x29C129C1u,0xAFA6AFA6u,0xAFA7AFA7u},0x00002D21u,0x971CBD47u,60,"osEepromLongRead #60"},
  {{0x3C023C02u,0x03E003E0u,0x8C428C42u,0x00000000u,0x00000000u,0x00000000u,0x00000000u,0x00000000u},0x00000000u,0x00CBEA0Fu,2,"osPiGetDeviceType  #2"},
  {{0x3C0E3C0Eu,0x03E003E0u,0x8DC28DC2u,0x00000000u,0x00000000u,0x00000000u,0x00000000u,0x00000000u},0x00000000u,0x3C534E60u,2,"osPiGetStatus  #2"},
  {{0x3C0E3C0Eu,0x8DC68DC6u,0x27BD27BDu,0x30CF30CFu,0x11E011E0u,0x00000000u,0x3C183C18u,0x8F068F06u},0x0000031Cu,0x106FF910u,2,"osPiRawReadIo  #2"},
  {{0x3C0E3C0Eu,0x8DC68DC6u,0x27BD27BDu,0x30CF30CFu,0x11E011E0u,0x00000000u,0x3C183C18u,0x8F068F06u},0x0000431Cu,0x106FF910u,2,"osPiRawWriteIo  #2"},
  {{0x27BD27BDu,0xAFBFAFBFu,0xAFA4AFA4u,0xAFA5AFA5u,0xAFA6AFA6u,0xAFA7AFA7u,0xAFB1AFB1u,0xAFB0AFB0u},0x00008C01u,0x83360A7Bu,2,"osPiRawStartDma #2"},
  {{0x27BD27BDu,0x3C0E3C0Eu,0x8DCE8DCEu,0xAFBFAFBFu,0xAFA4AFA4u,0xAFA5AFA5u,0xAFA6AFA6u,0x15C015C0u},0x00008085u,0xFA781B07u,2,"osCreatePiManager #2"},
  {{0x3C0E3C0Eu,0x8DC78DC7u,0x27BD27BDu,0x30EF30EFu,0x11E011E0u,0x00000000u,0x3C183C18u,0x8F078F07u},0x0000031Cu,0x6CEDB894u,2,"osEPiRawReadIo #2"},
  {{0x3C0E3C0Eu,0x8DC78DC7u,0x27BD27BDu,0x30EF30EFu,0x11E011E0u,0x00000000u,0x3C183C18u,0x8F078F07u},0x0000231Cu,0x6CEDB894u,2,"osEPiRawWriteIo  #2"},
  {{0x27BD27BDu,0xAFBFAFBFu,0xAFA4AFA4u,0xAFA5AFA5u,0xAFA6AFA6u,0xAFA7AFA7u,0xAFB1AFB1u,0xAFB0AFB0u},0x00008C01u,0x83360A7Bu,2,"osEPiRawStartDma  #2"},
  {{0x27BD27BDu,0xAFBFAFBFu,0xAFA4AFA4u,0xAFA5AFA5u,0xAFA6AFA6u,0x00000000u,0xAFB0AFB0u,0x8FA48FA4u},0x00000001u,0x0E6D59BBu,2,"osEPiWriteIo  #2"},
  {{0x27BD27BDu,0xAFBFAFBFu,0xAFA4AFA4u,0xAFA5AFA5u,0xAFA6AFA6u,0x00000000u,0xAFB0AFB0u,0x8FA48FA4u},0x00000001u,0x0E6D59BBu,2,"osEPiReadIo  #2"},
  {{0x27BD27BDu,0x3C0E3C0Eu,0x8DCE8DCEu,0xAFBFAFBFu,0xAFA4AFA4u,0xAFA5AFA5u,0xAFA6AFA6u,0xAFB1AFB1u},0x00000D05u,0xD9187407u,37,"osEPiStartDma #37"},
  {{0x3C0E3C0Eu,0x8DCE8DCEu,0x15C015C0u,0x00000000u,0x03E003E0u,0x00000000u,0x00000000u,0x00000000u},0x00000006u,0x00313A6Fu,2,"osPiGetCmdQueue  #2"},
  {{0x3C013C01u,0x27BD27BDu,0xA020A020u,0xAFBFAFBFu,0x3C013C01u,0x3C0E3C0Eu,0xAFA0AFA0u,0xAC2EAC2Eu},0x0000C186u,0x584E9288u,2,"osCartRomInit  #2"},
  {{0x240E240Eu,0x3C013C01u,0xA02EA02Eu,0x3C013C01u,0x3C0F3C0Fu,0xAC2FAC2Fu,0x3C013C01u,0x24182418u},0x0000EDA5u,0x385030D6u,2,"osLeoDiskInit  #2"},
  {{0x3C0E3C0Eu,0x8DCE8DCEu,0x27BD27BDu,0xAFBFAFBFu,0x15C015C0u,0x00000000u,0x10001000u,0x00000000u},0x00000A56u,0x6357077Cu,1,"__osLeoInterrupt  #1"},
  {{0x27BD27BDu,0xAFA4AFA4u,0x8FAE8FAEu,0xAFBFAFBFu,0xAFA0AFA0u,0xAFA0AFA0u,0xAFA0AFA0u,0xAFAEAFAEu},0x00008601u,0x1D4D8107u,1,"__osDevMgrMain #1"},
  {{0x27BD27BDu,0xAFBFAFBFu,0x240E240Eu,0x3C013C01u,0x3C043C04u,0x3C053C05u,0xAC2EAC2Eu,0x24A524A5u},0x000015C5u,0x434E72C3u,1,"__osPiCreateAccessQueue  #1"},
  {{0x3C0E3C0Eu,0x8DCE8DCEu,0x27BD27BDu,0xAFBFAFBFu,0x15C015C0u,0x00000000u,0x00000000u,0x00000000u},0x00005616u,0x274FB39Cu,1,"__osPiGetAccess  #1"},
  {{0x27BD27BDu,0xAFBFAFBFu,0x3C043C04u,0x24842484u,0x00000000u,0x00000000u,0x00000000u,0x8FBF8FBFu},0x00000109u,0x000D6BC3u,1,"__osPiRelAccess  #1"},
  {{0x27BD27BDu,0xAFBFAFBFu,0xAFA4AFA4u,0xAFA5AFA5u,0x00000000u,0xAFB0AFB0u,0x8FA48FA4u,0x00000000u},0x00008001u,0xDA1D0ABBu,2,"osPiWriteIo  #2"},
  {{0x27BD27BDu,0xAFBFAFBFu,0xAFA4AFA4u,0xAFA5AFA5u,0x00000000u,0xAFB0AFB0u,0x8FA48FA4u,0x00000000u},0x00008001u,0xDA1D0ABBu,2,"osPiReadIo  #2"},
  {{0x27BD27BDu,0x3C0E3C0Eu,0x8DCE8DCEu,0xAFBFAFBFu,0xAFA4AFA4u,0xAFA5AFA5u,0xAFA6AFA6u,0xAFA7AFA7u},0x00005A05u,0x1E15B407u,12,"osPiStartDma #12"},
  {{0x28C128C1u,0x14201420u,0x00850085u,0x30423042u,0x14401440u,0x00040004u,0x33183318u,0x13001300u},0x0000C0DBu,0xBB8C0CCEu,0,"bcmp"},
  {{0x10C010C0u,0x00A000A0u,0x10851085u,0x00A400A4u,0x54205420u,0x28C128C1u,0x00860086u,0x00A200A2u},0x0000BE25u,0xD1D5189Au,0,"bcopy"},
  {{0x28A128A1u,0x14201420u,0x00040004u,0x30633063u,0x10601060u,0x00A300A3u,0xA880A880u,0x00830083u},0x0000251Bu,0x5980BB1Du,0,"bzero"},
  {{0xAFA4AFA4u,0xAFA5AFA5u,0xAFA6AFA6u,0xAFA7AFA7u,0xDFAFDFAFu,0xDFAEDFAEu,0x01EE01EEu,0x00020002u},0x00000000u,0x000DDDB1u,0,"__ull_rshift"},
  {{0xAFA4AFA4u,0xAFA5AFA5u,0xAFA6AFA6u,0xAFA7AFA7u,0xDFAFDFAFu,0xDFAEDFAEu,0x01EE01EEu,0x00020002u},0x00000000u,0x000DDDB1u,0,"A__ull_rshift"},
  {{0xAFA4AFA4u,0xAFA5AFA5u,0xAFA6AFA6u,0xAFA7AFA7u,0xDFAFDFAFu,0xDFAEDFAEu,0x01EE01EEu,0x00020002u},0x00000000u,0x000DDDB1u,0,"A__ull_rshift"},
  {{0xAFA4AFA4u,0xAFA5AFA5u,0xAFA6AFA6u,0xAFA7AFA7u,0xDFAFDFAFu,0xDFAEDFAEu,0x01EE01EEu,0x00020002u},0x00000000u,0x000DDDB1u,0,"A__ull_rshift"},
  {{0xAFA4AFA4u,0xAFA5AFA5u,0xAFA6AFA6u,0xAFA7AFA7u,0xDFAFDFAFu,0xDFAEDFAEu,0x01CF01CFu,0x15E015E0u},0x00000080u,0x180E8DB1u,5,"__ull_rem #5"},
  {{0xAFA4AFA4u,0xAFA5AFA5u,0xAFA6AFA6u,0xAFA7AFA7u,0xDFAFDFAFu,0xDFAEDFAEu,0x01CF01CFu,0x15E015E0u},0x00000080u,0x180E8DB1u,5,"A__ull_rem #5"},
  {{0xAFA4AFA4u,0xAFA5AFA5u,0xAFA6AFA6u,0xAFA7AFA7u,0xDFAFDFAFu,0xDFAEDFAEu,0x01CF01CFu,0x15E015E0u},0x00000080u,0x180E8DB1u,5,"A__ull_rem #5"},
  {{0xAFA4AFA4u,0xAFA5AFA5u,0xAFA6AFA6u,0xAFA7AFA7u,0xDFAFDFAFu,0xDFAEDFAEu,0x01CF01CFu,0x15E015E0u},0x00000080u,0x180E8DB1u,5,"A__ull_rem #5"},
  {{0xAFA4AFA4u,0xAFA5AFA5u,0xAFA6AFA6u,0xAFA7AFA7u,0xDFAFDFAFu,0xDFAEDFAEu,0x01CF01CFu,0x15E015E0u},0x00000080u,0x182E8DB1u,3,"__ull_div #3"},
  {{0xAFA4AFA4u,0xAFA5AFA5u,0xAFA6AFA6u,0xAFA7AFA7u,0xDFAFDFAFu,0xDFAEDFAEu,0x01CF01CFu,0x15E015E0u},0x00000080u,0x182E8DB1u,3,"A__ull_div #3"},
  {{0xAFA4AFA4u,0xAFA5AFA5u,0xAFA6AFA6u,0xAFA7AFA7u,0xDFAFDFAFu,0xDFAEDFAEu,0x01CF01CFu,0x15E015E0u},0x00000080u,0x182E8DB1u,3,"A__ull_div #3"},
  {{0xAFA4AFA4u,0xAFA5AFA5u,0xAFA6AFA6u,0xAFA7AFA7u,0xDFAFDFAFu,0xDFAEDFAEu,0x01CF01CFu,0x15E015E0u},0x00000080u,0x182E8DB1u,3,"A__ull_div #3"},
  {{0xAFA4AFA4u,0xAFA5AFA5u,0xAFA6AFA6u,0xAFA7AFA7u,0xDFAFDFAFu,0xDFAEDFAEu,0x01EE01EEu,0x00020002u},0x00000000u,0x000DFDB1u,0,"__ll_lshift"},
  {{0xAFA4AFA4u,0xAFA5AFA5u,0xAFA6AFA6u,0xAFA7AFA7u,0xDFAFDFAFu,0xDFAEDFAEu,0x01EE01EEu,0x00020002u},0x00000000u,0x000DFDB1u,0,"A__ll_lshift"},
  {{0xAFA4AFA4u,0xAFA5AFA5u,0xAFA6AFA6u,0xAFA7AFA7u,0xDFAFDFAFu,0xDFAEDFAEu,0x01EE01EEu,0x00020002u},0x00000000u,0x000DFDB1u,0,"A__ll_lshift"},
  {{0xAFA4AFA4u,0xAFA5AFA5u,0xAFA6AFA6u,0xAFA7AFA7u,0xDFAFDFAFu,0xDFAEDFAEu,0x01EE01EEu,0x00020002u},0x00000000u,0x000DFDB1u,0,"A__ll_lshift"},
  {{0xAFA4AFA4u,0xAFA5AFA5u,0xAFA6AFA6u,0xAFA7AFA7u,0xDFAFDFAFu,0xDFAEDFAEu,0x01CF01CFu,0x15E015E0u},0x00000080u,0x180E8DB1u,5,"__ll_rem #5"},
  {{0xAFA4AFA4u,0xAFA5AFA5u,0xAFA6AFA6u,0xAFA7AFA7u,0xDFAFDFAFu,0xDFAEDFAEu,0x01CF01CFu,0x15E015E0u},0x00000080u,0x180E8DB1u,5,"A__ll_rem #5"},
  {{0xAFA4AFA4u,0xAFA5AFA5u,0xAFA6AFA6u,0xAFA7AFA7u,0xDFAFDFAFu,0xDFAEDFAEu,0x01CF01CFu,0x15E015E0u},0x00000080u,0x180E8DB1u,5,"A__ll_rem #5"},
  {{0xAFA4AFA4u,0xAFA5AFA5u,0xAFA6AFA6u,0xAFA7AFA7u,0xDFAFDFAFu,0xDFAEDFAEu,0x01CF01CFu,0x15E015E0u},0x00000080u,0x180E8DB1u,5,"A__ll_rem #5"},
  {{0xAFA4AFA4u,0xAFA5AFA5u,0xAFA6AFA6u,0xAFA7AFA7u,0xDFAFDFAFu,0xDFAEDFAEu,0x01CF01CFu,0x00000000u},0x00009100u,0xF28E9DB1u,6,"__ll_div #6"},
  {{0xAFA4AFA4u,0xAFA5AFA5u,0xAFA6AFA6u,0xAFA7AFA7u,0xDFAFDFAFu,0xDFAEDFAEu,0x01CF01CFu,0x00000000u},0x00009100u,0xF28E9DB1u,6,"A__ll_div #6"},
  {{0xAFA4AFA4u,0xAFA5AFA5u,0xAFA6AFA6u,0xAFA7AFA7u,0xDFAFDFAFu,0xDFAEDFAEu,0x01CF01CFu,0x00000000u},0x00009100u,0xF28E9DB1u,6,"A__ll_div #6"},
  {{0xAFA4AFA4u,0xAFA5AFA5u,0xAFA6AFA6u,0xAFA7AFA7u,0xDFAFDFAFu,0xDFAEDFAEu,0x01CF01CFu,0x00000000u},0x00009100u,0xF28E9DB1u,6,"A__ll_div #6"},
  {{0xAFA4AFA4u,0xAFA5AFA5u,0xAFA6AFA6u,0xAFA7AFA7u,0xDFAFDFAFu,0xDFAEDFAEu,0x01CF01CFu,0x00000000u},0x00000000u,0x00432DB1u,4,"__ll_mul #4"},
  {{0xAFA4AFA4u,0xAFA5AFA5u,0xAFA6AFA6u,0xAFA7AFA7u,0xDFAFDFAFu,0xDFAEDFAEu,0x01CF01CFu,0x00000000u},0x00000000u,0x00432DB1u,4,"A__ll_mul #4"},
  {{0xAFA4AFA4u,0xAFA5AFA5u,0xAFA6AFA6u,0xAFA7AFA7u,0xDFAFDFAFu,0xDFAEDFAEu,0x01CF01CFu,0x00000000u},0x00000000u,0x00432DB1u,4,"A__ll_mul #4"},
  {{0xAFA4AFA4u,0xAFA5AFA5u,0xAFA6AFA6u,0xAFA7AFA7u,0xDFAFDFAFu,0xDFAEDFAEu,0x01CF01CFu,0x00000000u},0x00000000u,0x00432DB1u,4,"A__ll_mul #4"},
  {{0x87AF87AFu,0xAFA6AFA6u,0xAFA7AFA7u,0xDFAEDFAEu,0x01E001E0u,0x03000300u,0x01D901D9u,0x17201720u},0x00000080u,0xD4AEE0B3u,0,"__ull_divremi"},
  {{0x87AF87AFu,0xAFA6AFA6u,0xAFA7AFA7u,0xDFAEDFAEu,0x01E001E0u,0x03000300u,0x01D901D9u,0x17201720u},0x00000080u,0xD4AEE0B3u,0,"A__ull_divremi"},
  {{0x87AF87AFu,0xAFA6AFA6u,0xAFA7AFA7u,0xDFAEDFAEu,0x01E001E0u,0x03000300u,0x01D901D9u,0x17201720u},0x00000080u,0xD4AEE0B3u,0,"A__ull_divremi"},
  {{0x87AF87AFu,0xAFA6AFA6u,0xAFA7AFA7u,0xDFAEDFAEu,0x01E001E0u,0x03000300u,0x01D901D9u,0x17201720u},0x00000080u,0xD4AEE0B3u,0,"A__ull_divremi"},
  {{0x27BD27BDu,0xAFA4AFA4u,0xAFA5AFA5u,0xAFA6AFA6u,0xAFA7AFA7u,0xDFAFDFAFu,0xDFAEDFAEu,0x01CF01CFu},0x00002201u,0xBC4E2F5Bu,0,"__ll_mod"},
  {{0x27BD27BDu,0xAFA4AFA4u,0xAFA5AFA5u,0xAFA6AFA6u,0xAFA7AFA7u,0xDFAFDFAFu,0xDFAEDFAEu,0x01CF01CFu},0x00002201u,0xBC4E2F5Bu,0,"A__ll_mod"},
  {{0x27BD27BDu,0xAFA4AFA4u,0xAFA5AFA5u,0xAFA6AFA6u,0xAFA7AFA7u,0xDFAFDFAFu,0xDFAEDFAEu,0x01CF01CFu},0x00002201u,0xBC4E2F5Bu,0,"A__ll_mod"},
  {{0x27BD27BDu,0xAFA4AFA4u,0xAFA5AFA5u,0xAFA6AFA6u,0xAFA7AFA7u,0xDFAFDFAFu,0xDFAEDFAEu,0x01CF01CFu},0x00002201u,0xBC4E2F5Bu,0,"A__ll_mod"},
  {{0xAFA4AFA4u,0xAFA5AFA5u,0xAFA6AFA6u,0xAFA7AFA7u,0xDFAFDFAFu,0xDFAEDFAEu,0x01EE01EEu,0x00020002u},0x00000000u,0x000DCDB1u,0,"__ll_rshift"},
  {{0xAFA4AFA4u,0xAFA5AFA5u,0xAFA6AFA6u,0xAFA7AFA7u,0xDFAFDFAFu,0xDFAEDFAEu,0x01EE01EEu,0x00020002u},0x00000000u,0x000DCDB1u,0,"A__ll_rshift"},
  {{0xAFA4AFA4u,0xAFA5AFA5u,0xAFA6AFA6u,0xAFA7AFA7u,0xDFAFDFAFu,0xDFAEDFAEu,0x01EE01EEu,0x00020002u},0x00000000u,0x000DCDB1u,0,"A__ll_rshift"},
  {{0xAFA4AFA4u,0xAFA5AFA5u,0xAFA6AFA6u,0xAFA7AFA7u,0xDFAFDFAFu,0xDFAEDFAEu,0x01EE01EEu,0x00020002u},0x00000000u,0x000DCDB1u,0,"A__ll_rshift"},
  {{0x27BD27BDu,0xAFA4AFA4u,0xAFBFAFBFu,0xAFA5AFA5u,0xAFA6AFA6u,0xAFA7AFA7u,0x3C043C04u,0x24842484u},0x00000181u,0xCA649E3Bu,0,"sprintf"},
  {{0x00800080u,0x10C010C0u,0x00A000A0u,0x906E906Eu,0x24C624C6u,0x24422442u,0x24632463u,0x14C014C0u},0x000000F2u,0x001E42BDu,0,"memcpy"},
  {{0x908E908Eu,0x00800080u,0x11C011C0u,0x00000000u,0x906F906Fu,0x24632463u,0x55E055E0u,0x906F906Fu},0x00000024u,0x0006FCD8u,0,"strlen"},
  {{0x90839083u,0x30AE30AEu,0x30A230A2u,0x51C351C3u,0x00800080u,0x54605460u,0x90839083u,0x03E003E0u},0x00000006u,0x00018CBEu,0,"strchr"},
  {{0x27BD27BDu,0xAFB7AFB7u,0xAFB6AFB6u,0xAFB5AFB5u,0xAFBEAFBEu,0xAFB4AFB4u,0xAFB3AFB3u,0xAFA7AFA7u},0x00000001u,0x486FA0F3u,0,"_Printf"},
  {{0x27BD27BDu,0xAFB1AFB1u,0x30A230A2u,0x24032403u,0x00800080u,0xAFBFAFBFu,0xAFB3AFB3u,0xAFB2AFB2u},0x0000B20Du,0x3A6991FFu,0,"_Litob"},
  {{0x27BD27BDu,0xAFA4AFA4u,0xAFBFAFBFu,0xAFA6AFA6u,0xAFA7AFA7u,0x00C000C0u,0x00E000E0u,0x8FA78FA7u},0x00000001u,0xD14D8F7Fu,0,"lldiv"},
  {{0x00A600A6u,0x00000000u,0x27BD27BDu,0x14C014C0u,0x00000000u,0x00070007u,0x24012401u,0x14C114C1u},0x000042CCu,0x500B111Eu,0,"ldiv"},
  {{0x27BD27BDu,0xAFBFAFBFu,0xAFB5AFB5u,0xAFB4AFB4u,0xAFB3AFB3u,0xAFB2AFB2u,0xAFB1AFB1u,0xAFB0AFB0u},0x00009001u,0xFEF33863u,0,"_Ldtob"},
  {{0xAFA4AFA4u,0xAFA5AFA5u,0xAFA6AFA6u,0xAFA7AFA7u,0xDFAFDFAFu,0xDFAEDFAEu,0x01CF01CFu,0x00000000u},0x00009100u,0xF28E964Bu,6,"__ll_div #6"},
  {{0xAFA4AFA4u,0xAFA5AFA5u,0xAFA6AFA6u,0xAFA7AFA7u,0xDFAFDFAFu,0xDFAEDFAEu,0x01CF01CFu,0x00000000u},0x00009100u,0xF28E964Bu,6,"A__ll_div #6"},
};
static constexpr size_t ULTRAHLE_OSCALL_N = 245;
static const char* ULTRAHLE_OSPATCH[] = {
  "osSpTaskLoad #39",
  "osSpTaskStartGo #22",
  "osSpTaskYield #46",
  "osSpTaskYielded #47",
  "__osSpSetStatus #1",
  "__osSpSetPc #1",
  "__osSpRawStartDma #1",
  "__osSpDeviceBusy #1",
  "__osSpGetStatus #1",
  "__ull_rshift #9",
  "__ull_rem #5",
  "__ull_div #3",
  "__ll_lshift #9",
  "__ll_rem #5",
  "__ll_div #6",
  "__ll_mul #4",
  "__ull_divremi #9",
  "__ll_mod #9",
  "__ll_rshift #9",
  "osSetTime #2",
  "osMapTLB #24",
  "osMapTLBRdb #2",
  "osCreateMesgQueue #13",
  "osSetEventMesg #16",
  "osViSetEvent #17",
  "osCreateThread #10",
  "osRecvMesg #14",
  "osViGetCurrentField #2",
  "osViGetCurrentLine #2",
  "osViGetStatus #2",
  "osSendMesg #15",
  "osStartThread #11",
  "osStopThread #28",
  "osWriteBackDCacheAll #1",
  "osCreateViManager #55",
  "osViSetMode #2",
  "osViBlack #2",
  "osViSetSpecialFeatures #2",
  "osCreatePiManager #2",
  "osSetThreadPri #18",
  "osInitialize #2",
  "osViSwapBuffer #23",
  "osViGetCurrentFramebuffer #20",
  "osViGetNextFramebuffer #20",
  "osContStartReadData #26",
  "osContGetReadData #27",
  "osContStartQuery #42",
  "osContGetQuery #43",
  "osContInit #25",
  "osEepromProbe #57",
  "osInvalDCache #1",
  "osInvalICache #38",
  "osPiStartDma #12",
  "osEPiStartDma #37",
  "osInvalCache #1",
  "osEepromLongRead #60",
  "osEepromLongWrite #61",
  "osGetTime #19",
  "osAiSetFrequency #32",
  "osWriteBackDCache #2",
  "osAiGetLength #31",
  "osAiSetNextBuffer #30",
  "osVirtualToPhysical #21",
  "osPhysicalToVirtual #29",
  "osGetThreadPri #2",
  "osGetCount #48",
  "osPiRawStartDma #2",
  "osMapTLBRdb #2",
  "osEPiRawReadIo #2",
  "osSetTimer #33",
  "osStopTimer #34",
  "osEepromWrite #59",
  "osJamMesg #15",
  "osEepromRead #58",
  "osSetIntMask #2",
  "__osDisableInt #1",
  "__osRestoreInt #1",
  "__ososViDevMgrMain #1",
  "__ososContGetInitData #1",
  "__ososPackRequestData #1",
  "__ososTimerServicesInit #1",
  "__ososTimerInterrupt #1",
  "__ososSetTimerIntr #1",
  "__ososInsertTimer #1",
  "__ososViInit #1",
  "__ososExceptionPreamble #1",
  "__ososEnqueueAndYield #1",
  "__ososEnqueueThread #1",
  "__ososPopThread #1",
  "__ososDispatchThread #1",
  "__ososCleanupThread #1",
  "__ososPiCreateAccessQueue #1",
  "__ososPiGetAccess #1",
  "__ososPiRelAccess #1",
  "__osDevMgrMain #1",
  "__osGetSR #1",
  "__osSetFpcCsr #1",
  "__osSiRawReadIo #1",
  "__osSiRawWriteIo #1",
  "__osSiCreateAccessQueue #1",
  "__osSiGetAccess #1",
  "__osSiRelAccess #1",
  "__osSiRawStartDma #1",
  "__osEepStatus #1",
  "__osAiDeviceBusy #1",
  "__osSetCompare #1",
  "__osProbeTLB #1",
  "__osSyncPutChars #1",
  "__osSiDeviceBusy #1",
  "__ososAtomicDec #1",
  "__osViDevMgr #1",
  "__osPiDevMgr #1",
  "__osRunQueue #1",
  "__osActiveQueue #1",
  "__osRunningThread #1",
  "__osViNext #1",
  "__osPiAccessQueueEnabled #1",
  "__osRcpImTable #1",
  "__osEventStateTab #1",
  "__osMyViThread #1",
  "__osMyViStack #1",
  "__osMyViQueue #1",
  "__osMyViMesg #1",
  "__osCurrentTime #1",
  "__osCurrentTime_2 #1",
  "__osBaseCounter #1",
  "__os* #1",
  "os* #2",
};
static constexpr size_t ULTRAHLE_OSPATCH_N = 128;
static const char* ULTRAHLE_DISABLE_PATCHES[] = {
  "osPiGetDeviceType",
  "osPiGetStatus",
  "osPiRawReadIo",
  "osPiRawWriteIo",
  "osPiRawStartDma",
  "osPiGetCmdQueue",
  "__osPiDevMgr",
  "__osPiTable",
  "__osPiAccessQueueEnabled",
  "__osPiCreateAccessQueue",
  "__osPiGetAccess",
  "__osPiRelAccess",
  "osPiWriteIo",
  "osPiReadIo",
  "__osPfsPifRam",
  "osPfsReFormat",
  "__osPfsRWInode",
  "__osPfsSelectBank",
  "osPfsInit",
  "__osPfsGetStatus",
  "osPfsAllocateFile",
  "__osPfsDeclearPage",
  "osPfsDeleteFile",
  "__osPfsReleasePages",
  "osPfsReadWriteFile",
  "osPfsFileState",
  "osPfsFindFile",
  "osPfsSetLabel",
  "osPfsGetLabel",
  "osPfsIsPlug",
  "__osPfsRequestData",
  "__osPfsGetInitData",
  "osPfsFreeBlocks",
  "osPfsNumFiles",
  "osPfsReSizeFile",
  "osContReset",
  "__osContRamWrite",
  "__osContAddressCrc",
  "__osContDataCrc",
  "__osContRamRead",
  "__osContPifRam",
  "__osContLastCmd",
  "__osContinitialized",
  "__osSiGetStatus",
  "__osSiRawReadIo",
  "__osSiDeviceBusy",
  "__osSiRawWriteIo",
  "__osSiRawStartDma",
  "__osSiCreateAccessQueue",
  "__osSiGetAccess",
  "__osSiRelAccess",
  "__osSiAccessQueue",
  "__osSiAccessQueueEnabled",
  "__osGetCause",
  "__osGetCompare",
  "__osGetConfig",
  "__osGetFpcCsr",
  "__osGetSR",
  "__osGetTLBASID",
  "__osGetTLBHi",
  "__osGetTLBLo0",
  "__osGetTLBLo1",
  "__osGetTLBPageMask",
  "__osSetCause",
  "__osSetCompare",
  "__osSetConfig",
  "__osSetCount",
  "__osSetFpcCsr",
  "__osSetSR",
};
static constexpr size_t ULTRAHLE_DISABLE_PATCHES_N = 69;


// ── DeviceBus ──
struct DeviceBus {
  ACsN64Core* core = nullptr;
  std::unordered_map<u32, u32> regs;
  u32 hw_interrupts = 0, mi_intr_mask = 0, mi_mode = 0, mi_version = 0x02020102;
  u32 sp_status = SP_STATUS_HALT, dpc_status = 0, half_line = 0;
  bool strict_tlb = false;
  std::unordered_map<u32, std::pair<u32,bool>> tlb_cache;
  DeviceBus(ACsN64Core* c) : core(c) { reset(); }
  void reset();
  u32 v_to_p(u32 addr, bool write=false);
  u8 read_u8(u32 addr);
  u16 read_u16(u32 addr);
  u32 read_u32(u32 addr);
  u64 read_u64(u32 addr) { return ((u64)read_u32(addr) << 32) | read_u32(addr + 4); }
  void write_u8(u32 addr, u8 val);
  void write_u16(u32 addr, u16 val);
  void write_u32(u32 addr, u32 val);
  void write_u64(u32 addr, u64 val) { write_u32(addr, (u32)(val >> 32)); write_u32(addr + 4, (u32)val); }
  u32 _read_mmio(u32 p);
  void _write_mmio(u32 p, u32 val);
  void _change_sp_status(u32 mv);
  void _change_dpc_status(u32 mv);
  void _change_mi_mode(u32 mv);
  void _change_mi_intr_mask(u32 mv);
};

// ── CPUCore ──
struct CPUCore {
  ACsN64Core* core = nullptr;
  u64 gpr[32]{}, fpr[32]{};
  u32 cp0[32]{};
  u32 fcr0 = 0x00000511, fcr31 = 0;
  u64 hi = 0, lo = 0;
  u64 cop2_latch = 0; // Project64-Legacy cop2LatchedValue
  u32 pc = 0, next_pc = 4;
  bool llbit = false; u32 lladdr = 0;
  TLBEntry tlb[32]{};
  bool in_delay = false;
  int count_per_op = 1;
  bool idle = false;
  u32 _patch_pc = 0;
  CPUCore(ACsN64Core* c) : core(c) { reset(); }
  void reset();
  void step();
  void execute(const N64Opcode& o);
  void _branch(u32 t) { next_pc = u32m(t); }
  void _skip_likely() { pc = u32m(pc + 4); next_pc = u32m(pc + 4); }
  void _write_tlb_entry(int index);
  void _raise_exception(int exc_code, u32 old_pc, bool refill=false, int ce=0);
  void _tlb_exception(const TLBException& e, u32 old_pc);
  bool _test_cop1_usable() { return (cp0[CP0_STATUS] & STATUS_CU1) != 0; }
  bool _test_cop2_usable() { return (cp0[CP0_STATUS] & STATUS_CU2) != 0; }
  bool _fp_exception_enabled(u32 cause_bits) const {
    // Legacy TEST_COP1_FP_EXCEPTION: enables |= UNIMP (always trap unimplemented)
    u32 enable = ((fcr31 >> 7) & 0x1F) | FCR31_CAUSE_UNIMP;
    return (cause_bits & enable) != 0;
  }
  u32 fget32(int i);
  void fset32(int i, u32 v);
  u64 fget64(int i);
  void fset64(int i, u64 v);
  void _clear_fp_cause() { fcr31 &= ~0x3F000u; }
  void _set_fp_cause(u32 c) { fcr31 &= ~0x3F000u; fcr31 |= (c & 0x3F) << 12; }
  void _set_fp_flags(u32 c) { fcr31 |= (c & 0x3F) << 2; }
};

// ── ACsN64Core ──
struct ACsN64Core {
  std::vector<u8> rdram, rom, pif_ram, rsp_dmem, rsp_imem;
  std::string rom_path;
  N64Header rom_header;
  int cic = CIC_NUS_6102, region = REGION_NTSC, ram_mb = 8;
  u32 rsp_pc = 0;
  bool audio_signal = false, _vi_origin_set = false;
  SaveManager save_mgr;
  CheatEngine cheat_engine;
  CPUCore cpu;
  DeviceBus bus;
  UltraHleOs ultrahle;
  UltraHleSym uh_sym;
  std::string uh_boot_report;
  bool running = false, lle = false, use_jit = false, boot_turbo = true, fb_lit = false, rsp_busy = false;
  u32 last_os_task = 0;
  u64 frame_count = 0, cycle_count = 0, cycles_per_frame = N64_CYCLES_PER_FRAME_NTSC;
  double frame_period = FRAME_PERIOD_NTSC;
  int interp_steps = 40000;
  int fixed_steps = -1;
  std::vector<u8> fb_ppm;
  std::vector<u8> fb_rgba; // packed RGBA8888 for GUI (w*h*4)
  int fb_w = 320, fb_h = 240;
  SoftRDP rdp;
  GfxHLE gfx;
  AudioHLE audio;
  BlockJIT jit;
  RSP rsp;
  std::unordered_map<std::string, int> unimpl;
  std::unordered_map<std::string, u64> events;
  ACsN64Core()
    : rdram(RDRAM_SIZE), pif_ram(PIF_RAM_SIZE), rsp_dmem(RSP_DMEM_SIZE), rsp_imem(RSP_IMEM_SIZE),
      cpu(this), bus(this), ultrahle(this), uh_sym(this),
      rdp(this), gfx(this, &rdp), audio(this), jit(this), rsp(this) {
    use_jit = false;
    reset();
  }
  void reset();
  void note_unimpl(const std::string& k) { unimpl[k]++; }
  std::string load_rom(const std::string& path, bool lle_mode = false);
  void _hle_ipl3_boot();
  int step_frame(double budget_s = INTERP_FRAME_BUDGET_S);
  bool render_vi(int scale = 1);
  void process_rsp();
  void process_rdp(u32 data_ptr = 0, u32 data_size = 0, u32 ucode_data = 0, u32 ucode_data_size = 0);
  void process_audio() { audio_signal = true; bus.hw_interrupts |= MI_INTR_AI; }
  void process_audio_task(u32, u32) { process_audio(); }
  void trigger_sp_dma(bool to_rsp);
  void trigger_pi_dma();
  void trigger_si_dma(bool read_pif);
  bool _pif_control();
  void _pif_process_commands();
  u32 vi_current_line() { return half_line_proxy(); }
  u32 half_line_proxy() { return bus.half_line; }
  u32 ai_status() { return 0; }
  u32 ai_len_remaining() { return 0; }
  void ai_push(u32, u32) {}
  void _maybe_flush_saves() {}
};

DeviceBus* UltraHleOs::_bus() { return &core->bus; }
u32 UltraHleOs::_rd32(u32 addr) { return _bus()->read_u32(u32m(addr)); }
void UltraHleOs::_wr32(u32 addr, u32 val) { _bus()->write_u32(u32m(addr), u32m(val)); }
u32 UltraHleOs::_sp_arg(u64* g, int n) { return _rd32(u32m(g[_UH_SP] + 0x10 + n * 4)); }
void UltraHleOs::install_patch(u32 vaddr, int patch_id) {
  _wr32(vaddr, make_ultrahle_patch(patch_id));
}
void UltraHleOs::reset() {
  queues.clear(); queue_by_addr.clear(); queuenum = 1;
  event_mq.clear(); event_msg.clear(); fb_current = fb_next = 0;
  threads.clear(); thread_by_addr.clear(); current_thread = 0; threadnum = 1;
  timers.clear(); sptaskload = false; time_lo = time_hi = 0;
  cont_status = 0x0500; cont_pad = 0; ai_freq = 32000; ai_buf = ai_len = 0;
  ismario = iszelda = bootloader = block_pc = pending_switch = preempt_tid = 0;
  UhThread boot; boot.id=0; boot.active=1; boot.ready=1; boot.pri=0; boot.next_pc=4;
  threads[0] = boot;
}
void UltraHleOs::_save_thread_cpu(int tid) {
  auto it = threads.find(tid); if (it == threads.end()) return;
  auto& t = it->second; auto& cpu = core->cpu;
  t.pc = u32m(cpu.pc); t.next_pc = u32m(cpu.next_pc);
  std::memcpy(t.gpr.data(), cpu.gpr, sizeof(cpu.gpr)); t.hi = cpu.hi; t.lo = cpu.lo;
}
void UltraHleOs::_load_thread_cpu(int tid) {
  auto& t = threads.at(tid); auto& cpu = core->cpu;
  std::memcpy(cpu.gpr, t.gpr.data(), sizeof(cpu.gpr)); cpu.gpr[0] = 0; cpu.hi = t.hi; cpu.lo = t.lo;
  cpu.pc = u32m(t.pc); cpu.next_pc = u32m(t.next_pc); current_thread = tid;
}
int UltraHleOs::_find_ready_thread(int prefer) {
  int best = -1, best_pri = -1;
  int start = prefer ? prefer : current_thread;
  for (int i = 0; i < ULTRAHLE_MAX_THREAD * 2; i++) {
    int tid = (start + 1 + i) & (ULTRAHLE_MAX_THREAD - 1);
    auto it = threads.find(tid); if (it == threads.end() || !it->second.ready) continue;
    if (it->second.pri > best_pri) { best = tid; best_pri = it->second.pri; }
  }
  return best;
}
void UltraHleOs::switch_to(int tid) {
  if (tid == current_thread || !threads.count(tid)) return;
  _save_thread_cpu(current_thread); _load_thread_cpu(tid);
}
bool UltraHleOs::schedule(int prefer) {
  auto cur_it = threads.find(current_thread);
  bool cur_ready = cur_it != threads.end() && cur_it->second.ready;
  int cur_pri = cur_ready ? cur_it->second.pri : -1;
  int t = _find_ready_thread(prefer);
  if (prefer && threads.count(prefer) && threads[prefer].ready) {
    if (t < 0 || threads[prefer].pri >= threads[t].pri) t = prefer;
  }
  if (t >= 0 && t != current_thread) {
    int t_pri = threads[t].pri;
    if (!cur_ready || t_pri > cur_pri) { switch_to(t); return true; }
  }
  return false;
}
void UltraHleOs::block_current(int reason_q, bool send) {
  auto it = threads.find(current_thread); if (it == threads.end()) return;
  it->second.ready = 0;
  if (send) it->second.sendblock = reason_q; else it->second.recvblock = reason_q;
  schedule(0);
}
void UltraHleOs::unblock_waiters(int qid, bool recv) {
  for (auto& kv : threads) {
    auto& t = kv.second; int tid = kv.first;
    if (recv && t.recvblock == qid) { t.recvblock = 0; t.ready = 1; _request_preempt(tid); return; }
    if (!recv && t.sendblock == qid) { t.sendblock = 0; t.ready = 1; _request_preempt(tid); return; }
  }
}
void UltraHleOs::_request_preempt(int tid) {
  auto it = threads.find(tid); if (it == threads.end() || !it->second.ready) return;
  auto cur = threads.find(current_thread);
  int cur_pri = cur != threads.end() ? cur->second.pri : -1;
  if (it->second.pri > cur_pri) preempt_tid = tid;
}
bool UltraHleOs::maybe_preempt() {
  int tid = preempt_tid; preempt_tid = 0;
  if (tid && tid != current_thread && threads.count(tid) && threads[tid].ready)
    return schedule(tid);
  auto cur = threads.find(current_thread);
  int cur_pri = cur != threads.end() ? cur->second.pri : -1;
  int best = _find_ready_thread();
  if (best >= 0 && best != current_thread && threads[best].pri > cur_pri) return schedule(best);
  return false;
}
bool UltraHleOs::yield_fair() {
  std::vector<int> ready;
  for (auto& kv : threads) if (kv.second.ready && kv.second.pri > 0) ready.push_back(kv.first);
  if (ready.size() < 2) return false;
  std::sort(ready.begin(), ready.end());
  auto it = std::find(ready.begin(), ready.end(), current_thread);
  int nxt = (it == ready.end()) ? ready[0] : ready[(size_t)((it - ready.begin() + 1) % (int)ready.size())];
  if (nxt != current_thread) { switch_to(nxt); return true; }
  return false;
}
void UltraHleOs::_queue_to_mem(int qid) {
  auto& q = queues[qid];
  _wr32(q.memaddr + 0x00, (u32)qid);
  _wr32(q.memaddr + 0x04, ULTRAHLE_MQ_MAGIC);
  _wr32(q.memaddr + 0x08, (u32)q.num);
  _wr32(q.memaddr + 0x10, (u32)q.size);
}
void UltraHleOs::os_create_mesg_queue(u32 mq, u32 msg, int count) {
  mq = u32m(mq); msg = u32m(msg); count = std::max(0, (int)sign32((u32)count));
  int qid = queue_by_addr.count(mq) ? queue_by_addr[mq] : 0;
  if (!qid) { qid = queuenum; queuenum = std::min(queuenum + 1, ULTRAHLE_MAX_QUEUE - 1); if (qid <= 0) qid = 1; }
  UhQueue q; q.memaddr = mq; q.msg = msg; q.size = std::max(1, count); q.num = 0; q.pos = 0;
  q.data.assign((size_t)q.size, 0);
  queues[qid] = q; queue_by_addr[mq] = qid; _queue_to_mem(qid);
}
int UltraHleOs::_qid_from_mq(u32 mq) {
  mq = u32m(mq); int qid = (int)_rd32(mq);
  if (queues.count(qid) && queues[qid].memaddr == mq) return qid;
  if (queue_by_addr.count(mq)) return queue_by_addr[mq];
  return 0;
}
int UltraHleOs::os_send_mesg(u32 mq, u32 mesg, int block) {
  int qid = _qid_from_mq(mq); if (!qid || !queues.count(qid)) return 0;
  auto& q = queues[qid];
  if (q.num >= q.size) {
    if (block == OS_MESG_NOBLOCK || block == -1) {
      if (q.size > 0) q.num -= 1; else return -1;
    } else {
      block_pc = 1;
      if (threads.count(current_thread)) threads[current_thread].sendblock = qid;
      return 1;
    }
  }
  q.data[(size_t)q.pos] = u32m(mesg); q.num += 1; q.pos = (q.pos + 1) % q.size;
  _queue_to_mem(qid); unblock_waiters(qid, true); return 0;
}
int UltraHleOs::os_recv_mesg(u32 mq, u32 msg_ptr, int block) {
  int qid = _qid_from_mq(mq);
  if (!qid || !queues.count(qid)) {
    if (block == OS_MESG_NOBLOCK) return -1;
    block_pc = 1; return 1;
  }
  auto& q = queues[qid];
  if (q.num <= 0) {
    if (block == OS_MESG_NOBLOCK) return -1;
    block_pc = 1;
    if (threads.count(current_thread)) threads[current_thread].recvblock = qid;
    return 1;
  }
  int pos = q.pos - q.num; if (pos < 0) pos += q.size;
  u32 mesg = q.data[(size_t)pos]; q.num -= 1; _queue_to_mem(qid);
  if (msg_ptr) _wr32(msg_ptr, u32m(mesg));
  unblock_waiters(qid, false); return 0;
}
void UltraHleOs::os_set_event_mesg(int event, u32 mq, u32 mesg) {
  event &= 0xFF; event_mq[event] = u32m(mq); event_msg[event] = u32m(mesg);
}
void UltraHleOs::os_event(int event) {
  auto it = event_mq.find(event); if (it != event_mq.end())
    os_send_mesg(it->second, event_msg.count(event) ? event_msg[event] : 0, OS_MESG_NOBLOCK);
}
void UltraHleOs::tick_time(u64 cycles) {
  u32 c = u32m(cycles); time_lo = u32m(time_lo + c);
  if (time_lo < (c & (u32)MASK_32)) time_hi = u32m(time_hi + 1);
  _fire_timers(u32m(core->cpu.cp0[CP0_COUNT]));
}
void UltraHleOs::_fire_timers(u32 count) {
  count = u32m(count);
  for (auto& kv : timers) {
    auto& t = kv.second;
    if (!t.active) continue;
    if (u32m(count - t.expire) > 0x7FFFFFFFu) continue;
    if (t.mq) os_send_mesg(t.mq, t.msg, OS_MESG_NOBLOCK);
    if (t.interval) t.expire = u32m(count + t.interval); else t.active = false;
  }
}
u32 UltraHleOs::virt_to_phys(u32 addr) { return _bus()->v_to_p(u32m(addr)) & 0x1FFFFFFF; }
u32 UltraHleOs::phys_to_virt(u32 addr) { return u32m(0x80000000u | (u32m(addr) & 0x1FFFFFFF)); }
void UltraHleOs::create_thread(u32 m_thread, u32 tid, u32 entry, u32 arg, u32 stack, u32 pri) {
  m_thread = u32m(m_thread); tid = u32m(tid) & 0xFF;
  if (tid == 0 || tid >= (u32)ULTRAHLE_MAX_THREAD || (threads.count((int)tid) && threads[(int)tid].active)) {
    for (int i = 1; i < ULTRAHLE_MAX_THREAD; i++)
      if (!threads.count(i) || !threads[i].active) { tid = (u32)i; break; }
  }
  UhThread t; t.id = (int)tid; t.memaddr = m_thread; t.active = 1; t.ready = 0;
  t.pri = (int)pri * 10; t.pc = u32m(entry); t.next_pc = u32m(entry + 4);
  t.gpr.fill(0); t.gpr[4] = u64m(arg); t.gpr[29] = u64m(u32m(stack) - 8); t.gpr[31] = u64m(ULTRAHLE_THREAD_RA);
  threads[(int)tid] = t; thread_by_addr[m_thread] = (int)tid; _wr32(m_thread, tid);
  if ((int)tid >= threadnum) threadnum = (int)tid + 1;
}
void UltraHleOs::start_thread(u32 m_thread) {
  m_thread = u32m(m_thread); int tid = (int)_rd32(m_thread);
  if (!threads.count(tid) && thread_by_addr.count(m_thread)) tid = thread_by_addr[m_thread];
  if (!threads.count(tid)) return;
  threads[tid].ready = 1;
  auto& cpu = core->cpu; u32 ra = u32m(cpu.gpr[_UH_RA]);
  cpu.pc = ra; cpu.next_pc = u32m(ra + 4);
  if (threads.count(current_thread)) {
    auto& cur = threads[current_thread];
    cur.pc = ra; cur.next_pc = u32m(ra + 4); std::memcpy(cur.gpr.data(), cpu.gpr, sizeof(cpu.gpr)); cur.hi = cpu.hi; cur.lo = cpu.lo;
  }
  pending_switch = tid; schedule(tid);
}
void UltraHleOs::stop_current_thread() {
  if (threads.count(current_thread)) { threads[current_thread].active = 0; threads[current_thread].ready = 0; }
  schedule(0);
}

void DeviceBus::reset() {
  regs.clear(); hw_interrupts = 0; mi_intr_mask = 0; mi_mode = 0;
  sp_status = SP_STATUS_HALT; dpc_status = 0; half_line = 0; tlb_cache.clear();
  regs[VI_ORIGIN] = 0; regs[VI_WIDTH] = 320; regs[VI_V_CURRENT] = 0x3FF; regs[VI_INTR] = 0x3FF;
  core->_vi_origin_set = false;
}

u32 DeviceBus::v_to_p(u32 addr, bool) {
  addr &= (u32)MASK_32;
  if (addr >= 0x80000000u && addr < 0xC0000000u) return addr & 0x1FFFFFFF;
  // legacy identity fallback (non-strict)
  return addr & 0x1FFFFFFF;
}

u8 DeviceBus::read_u8(u32 addr) {
  u32 p = v_to_p(addr);
  if (p < RDRAM_SIZE) return core->rdram[p];
  if (p >= 0x04000000 && p < 0x04002000) {
    u32 off = p - 0x04000000;
    auto& buf = (off < 0x1000) ? core->rsp_dmem : core->rsp_imem;
    return buf[off & 0xFFF];
  }
  if (p >= 0x10000000 && p < 0x10000000 + core->rom.size()) return core->rom[p - 0x10000000];
  if (p >= 0x1FC007C0 && p < 0x1FC007C0 + PIF_RAM_SIZE) return core->pif_ram[p - 0x1FC007C0];
  return 0;
}
u16 DeviceBus::read_u16(u32 addr) {
  u32 p = v_to_p(addr);
  if (p + 1 < RDRAM_SIZE) return (u16)((core->rdram[p] << 8) | core->rdram[p+1]);
  return 0;
}
u32 DeviceBus::read_u32(u32 addr) {
  if ((addr & 0x1FFFFFFF) == (ULTRAHLE_OS_SEG & 0x1FFFFFFF)) return make_ultrahle_patch(28);
  u32 p = v_to_p(addr);
  if (p + 3 < RDRAM_SIZE)
    return (u32(core->rdram[p])<<24)|(u32(core->rdram[p+1])<<16)|(u32(core->rdram[p+2])<<8)|u32(core->rdram[p+3]);
  if (p >= 0x04000000 && p < 0x04002000) {
    u32 off = p & 0xFFF;
    auto& buf = (p < 0x04001000) ? core->rsp_dmem : core->rsp_imem;
    return (u32(buf[off])<<24)|(u32(buf[off+1])<<16)|(u32(buf[off+2])<<8)|u32(buf[off+3]);
  }
  if (p >= 0x04040000 && p <= 0x048FFFFF) return _read_mmio(p);
  if (p >= 0x08000000 && p < 0x10000000) return core->save_mgr.cart2_read32(p - 0x08000000);
  u32 roff = p - 0x10000000;
  if (roff + 3 < core->rom.size())
    return (u32(core->rom[roff])<<24)|(u32(core->rom[roff+1])<<16)|(u32(core->rom[roff+2])<<8)|u32(core->rom[roff+3]);
  return 0;
}
void DeviceBus::write_u8(u32 addr, u8 val) {
  u32 p = v_to_p(addr, true); val &= 0xFF;
  if (p < RDRAM_SIZE) { core->rdram[p] = val; return; }
  if (p >= 0x04000000 && p < 0x04002000) {
    auto& buf = (p < 0x04001000) ? core->rsp_dmem : core->rsp_imem;
    buf[p & 0xFFF] = val; return;
  }
  if (p >= 0x1FC007C0 && p < 0x1FC007C0 + PIF_RAM_SIZE) core->pif_ram[p - 0x1FC007C0] = val;
}
void DeviceBus::write_u16(u32 addr, u16 val) {
  u32 p = v_to_p(addr, true);
  if (p + 1 < RDRAM_SIZE) {
    core->rdram[p] = (val >> 8) & 0xFF; core->rdram[p+1] = val & 0xFF; return;
  }
}
void DeviceBus::write_u32(u32 addr, u32 val) {
  u32 p = v_to_p(addr, true); val &= (u32)MASK_32;
  if (p + 3 < RDRAM_SIZE) {
    core->rdram[p]=(val>>24)&0xFF; core->rdram[p+1]=(val>>16)&0xFF;
    core->rdram[p+2]=(val>>8)&0xFF; core->rdram[p+3]=val&0xFF; return;
  }
  if (p >= 0x04000000 && p < 0x04002000) {
    auto& buf = (p < 0x04001000) ? core->rsp_dmem : core->rsp_imem;
    u32 off = p & 0xFFF;
    buf[off]=(val>>24)&0xFF; buf[off+1]=(val>>16)&0xFF; buf[off+2]=(val>>8)&0xFF; buf[off+3]=val&0xFF; return;
  }
  if (p >= 0x04040000 && p <= 0x048FFFFF) _write_mmio(p, val);
  else if (p >= 0x08000000 && p < 0x10000000) core->save_mgr.cart2_write32(p - 0x08000000, val);
}

u32 DeviceBus::_read_mmio(u32 p) {
  u32 al = p & ~3u;
  if (al == SP_STATUS) return sp_status;
  if (al == SP_DMA_FULL || al == SP_DMA_BUSY) return 0;
  if (al == SP_PC) return core->rsp_pc;
  if (al == DPC_STATUS) return dpc_status;
  if (al == MI_MODE) return mi_mode;
  if (al == MI_VERSION) return mi_version;
  if (al == MI_INTR) return hw_interrupts;
  if (al == MI_INTR_MASK) return mi_intr_mask;
  if (al == VI_V_CURRENT) return half_line;
  auto it = regs.find(al); return it == regs.end() ? 0 : it->second;
}

void DeviceBus::_change_sp_status(u32 mv) {
  if (mv & SP_CLR_HALT) sp_status &= ~SP_STATUS_HALT;
  if (mv & SP_SET_HALT) sp_status |= SP_STATUS_HALT;
  if (mv & SP_CLR_BROKE) sp_status &= ~SP_STATUS_BROKE;
  if (mv & SP_CLR_INTR) hw_interrupts &= ~MI_INTR_SP;
  if (mv & SP_SET_INTR) hw_interrupts |= MI_INTR_SP;
  if (!(sp_status & SP_STATUS_HALT) && !core->rsp_busy) core->process_rsp();
}
void DeviceBus::_change_dpc_status(u32 mv) {
  if (mv & DPC_CLR_XBUS_DMEM_DMA) dpc_status &= ~DPC_STATUS_XBUS_DMEM_DMA;
  if (mv & DPC_SET_XBUS_DMEM_DMA) dpc_status |= DPC_STATUS_XBUS_DMEM_DMA;
  if (mv & DPC_CLR_FREEZE) dpc_status &= ~DPC_STATUS_FREEZE;
  if (mv & DPC_SET_FREEZE) dpc_status |= DPC_STATUS_FREEZE;
}
void DeviceBus::_change_mi_mode(u32 mv) {
  mi_mode = (mi_mode & ~0x7Fu) | (mv & 0x7F);
  if (mv & MI_CLR_INIT) mi_mode &= ~MI_MODE_INIT;
  if (mv & MI_SET_INIT) mi_mode |= MI_MODE_INIT;
  if (mv & MI_CLR_EBUS) mi_mode &= ~MI_MODE_EBUS;
  if (mv & MI_SET_EBUS) mi_mode |= MI_MODE_EBUS;
  if (mv & MI_CLR_DP_INTR) hw_interrupts &= ~MI_INTR_DP;
  if (mv & MI_CLR_RDRAM) mi_mode &= ~MI_MODE_RDRAM;
  if (mv & MI_SET_RDRAM) mi_mode |= MI_MODE_RDRAM;
}
void DeviceBus::_change_mi_intr_mask(u32 mv) {
  if (mv & MI_INTR_MASK_CLR_SP) mi_intr_mask &= ~MI_INTR_SP;
  if (mv & MI_INTR_MASK_SET_SP) mi_intr_mask |= MI_INTR_SP;
  if (mv & MI_INTR_MASK_CLR_SI) mi_intr_mask &= ~MI_INTR_SI;
  if (mv & MI_INTR_MASK_SET_SI) mi_intr_mask |= MI_INTR_SI;
  if (mv & MI_INTR_MASK_CLR_AI) mi_intr_mask &= ~MI_INTR_AI;
  if (mv & MI_INTR_MASK_SET_AI) mi_intr_mask |= MI_INTR_AI;
  if (mv & MI_INTR_MASK_CLR_VI) mi_intr_mask &= ~MI_INTR_VI;
  if (mv & MI_INTR_MASK_SET_VI) mi_intr_mask |= MI_INTR_VI;
  if (mv & MI_INTR_MASK_CLR_PI) mi_intr_mask &= ~MI_INTR_PI;
  if (mv & MI_INTR_MASK_SET_PI) mi_intr_mask |= MI_INTR_PI;
  if (mv & MI_INTR_MASK_CLR_DP) mi_intr_mask &= ~MI_INTR_DP;
  if (mv & MI_INTR_MASK_SET_DP) mi_intr_mask |= MI_INTR_DP;
}
void DeviceBus::_write_mmio(u32 p, u32 val) {
  u32 al = p & ~3u;
  if (al == SP_RD_LEN) { regs[al] = val; core->trigger_sp_dma(true); }
  else if (al == SP_WR_LEN) { regs[al] = val; core->trigger_sp_dma(false); }
  else if (al == SP_PC) { regs[al] = val; core->rsp_pc = val; }
  else if (al == SP_STATUS) _change_sp_status(val);
  else if (al == DPC_STATUS) _change_dpc_status(val);
  else if (al == DPC_END) { core->process_rdp(); regs[DPC_CURRENT] = val; regs[al] = val; }
  else if (al == MI_MODE) _change_mi_mode(val);
  else if (al == MI_INTR) hw_interrupts &= ~val;
  else if (al == MI_INTR_MASK) _change_mi_intr_mask(val);
  else if (al == VI_ORIGIN) { regs[al] = val; if (val & 0xFFFFFF) { core->_vi_origin_set = true; core->render_vi(); } }
  else if (al == VI_INTR) { regs[al] = val & 0x3FF; half_line = 0; }
  else if (al == VI_V_CURRENT) hw_interrupts &= ~MI_INTR_VI;
  else if (al == AI_LEN) { regs[al] = val; core->process_audio(); }
  else if (al == AI_STATUS) hw_interrupts &= ~MI_INTR_AI;
  else if (al == PI_RD_LEN) { regs[al] = val; core->trigger_pi_dma(); }
  else if (al == PI_WR_LEN) { regs[al] = val; core->trigger_pi_dma(); }
  else if (al == PI_STATUS) { if (val & 2) hw_interrupts &= ~MI_INTR_PI; }
  else if (al == SI_STATUS) hw_interrupts &= ~MI_INTR_SI;
  else if (al == SI_PIF_ADDR_RD) core->trigger_si_dma(true);
  else if (al == SI_PIF_ADDR_WR) core->trigger_si_dma(false);
  else regs[al] = val;
}

void CPUCore::reset() {
  std::memset(gpr, 0, sizeof(gpr)); std::memset(fpr, 0, sizeof(fpr)); std::memset(cp0, 0, sizeof(cp0));
  fcr0 = 0x00000511; fcr31 = 0; hi = lo = 0; cop2_latch = 0; pc = 0; next_pc = 4;
  cp0[CP0_PRID] = 0x00000B00; cp0[CP0_STATUS] = 0x34000000; cp0[CP0_CONFIG] = 0x7006E463;
  cp0[CP0_CONTEXT] = 0x007FFFF0; cp0[CP0_EPC] = 0xFFFFFFFF; cp0[CP0_BADVADDR] = 0xFFFFFFFF;
  cp0[CP0_ERROREPC] = 0xFFFFFFFF; cp0[CP0_CAUSE] = 0xB000005C;
  llbit = false; lladdr = 0;
  for (auto& e : tlb) e = TLBEntry{};
}

u32 CPUCore::fget32(int i) {
  if ((cp0[CP0_STATUS] & STATUS_FR) || !(i & 1)) return (u32)(fpr[i] & MASK_32);
  return (u32)(fpr[i - 1] >> 32);
}
void CPUCore::fset32(int i, u32 v) {
  v &= (u32)MASK_32;
  if ((cp0[CP0_STATUS] & STATUS_FR) || !(i & 1)) fpr[i] = (fpr[i] & 0xFFFFFFFF00000000ULL) | v;
  else fpr[i - 1] = (fpr[i - 1] & MASK_32) | ((u64)v << 32);
}
u64 CPUCore::fget64(int i) { return fpr[(cp0[CP0_STATUS] & STATUS_FR) ? i : (i & ~1)]; }
void CPUCore::fset64(int i, u64 v) { fpr[(cp0[CP0_STATUS] & STATUS_FR) ? i : (i & ~1)] = v & MASK_64; }

void CPUCore::_write_tlb_entry(int index) {
  core->bus.tlb_cache.clear(); core->jit.invalidate_mapped();
  int idx = index % 32; u32 hi = cp0[CP0_ENTRYHI], lo0 = cp0[CP0_ENTRYLO0], lo1 = cp0[CP0_ENTRYLO1];
  tlb[idx].mask = cp0[CP0_PAGEMASK]; tlb[idx].vpn2 = (hi >> 13) & 0x7FFFF; tlb[idx].asid = hi & 0xFF;
  tlb[idx].g = (lo0 & 1) && (lo1 & 1);
  tlb[idx].pfn0 = (lo0 >> 6) & 0xFFFFF; tlb[idx].c0 = (lo0 >> 3) & 7;
  tlb[idx].d0 = (lo0 >> 2) & 1; tlb[idx].v0 = (lo0 >> 1) & 1;
  tlb[idx].pfn1 = (lo1 >> 6) & 0xFFFFF; tlb[idx].c1 = (lo1 >> 3) & 7;
  tlb[idx].d1 = (lo1 >> 2) & 1; tlb[idx].v1 = (lo1 >> 1) & 1;
}

void CPUCore::_raise_exception(int exc_code, u32 old_pc, bool refill, int ce) {
  u32 cause = (cp0[CP0_CAUSE] & ~(0x7Cu | 0x30000000u)) | ((exc_code & 0x1F) << 2) | ((ce & 3) << 28);
  u32 status = cp0[CP0_STATUS];
  u32 off;
  if (!(status & STATUS_EXL)) {
    if (in_delay) { cp0[CP0_EPC] = u32m(old_pc - 4); cause |= CAUSE_BD; }
    else { cp0[CP0_EPC] = u32m(old_pc); cause &= ~CAUSE_BD; }
    off = refill ? 0x000 : 0x180;
  } else off = 0x180;
  cp0[CP0_CAUSE] = cause; cp0[CP0_STATUS] = status | STATUS_EXL;
  u32 base = (status & STATUS_BEV) ? 0xBFC00200u : 0x80000000u;
  pc = base + off; next_pc = pc + 4; in_delay = false;
}
void CPUCore::_tlb_exception(const TLBException& e, u32 old_pc) {
  u32 va = e.vaddr & (u32)MASK_32;
  cp0[CP0_BADVADDR] = va;
  cp0[CP0_CONTEXT] = (cp0[CP0_CONTEXT] & 0xFF800000) | ((va >> 9) & 0x007FFFF0);
  cp0[CP0_ENTRYHI] = (va & 0xFFFFE000) | (cp0[CP0_ENTRYHI] & 0xFF);
  _raise_exception(e.code, old_pc, e.refill);
}

void CPUCore::step() {
  u32 status = cp0[CP0_STATUS];
  u32 pending_rcp = core->bus.hw_interrupts & core->bus.mi_intr_mask & 0x3F;
  if (pending_rcp) cp0[CP0_CAUSE] = (cp0[CP0_CAUSE] & ~CAUSE_IP2) | CAUSE_IP2;
  else cp0[CP0_CAUSE] &= ~CAUSE_IP2;
  if ((status & STATUS_IE) && !(status & (STATUS_EXL | STATUS_ERL))) {
    if ((cp0[CP0_CAUSE] & status & 0xFF00) != 0) {
      in_delay = next_pc != ((pc + 4) & (u32)MASK_32);
      _raise_exception(0, pc);
    }
  }
  u32 word = core->bus.read_u32(pc);
  execute(get_opcode(word));
  gpr[0] = 0;
  u32 old = cp0[CP0_COUNT];
  int cpo = count_per_op;
  cp0[CP0_COUNT] = (old + cpo) & (u32)MASK_32;
  if (((cp0[CP0_COMPARE] - old - 1) & (u32)MASK_32) < (u32)cpo)
    cp0[CP0_CAUSE] |= CAUSE_IP7;
}

void CPUCore::execute(const N64Opcode& o) {
  u32 old_pc = pc;
  u32 npc = next_pc;
  in_delay = npc != ((old_pc + 4) & (u32)MASK_32);
  pc = npc;
  next_pc = (npc + 4) & (u32)MASK_32;
  HandlerFn h = (o.instr_id < DISPATCH_SIZE) ? _DISPATCH[o.instr_id] : nullptr;
  if (h) {
    try { h(this, o, old_pc, gpr); }
    catch (const TLBException& e) { _tlb_exception(e, old_pc); }
  } else if (o.op == 0x10 || o.op == 0x11 || o.op == 0x12 || o.op == 0x13) {
    core->note_unimpl("cpu:CpU");
    _raise_exception(11, old_pc, false, o.op & 3);
  } else {
    core->note_unimpl("cpu:RI");
    _raise_exception(10, old_pc);
  }
}


// ── Instruction handlers ──
static void _h_NOP(CPUCore*, const N64Opcode&, u32, u64*) {}
static void _h_LUI(CPUCore*, const N64Opcode& o, u32, u64* g) { g[o.rt] = sx32_to_64((u64)o.imm << 16); }
static void _h_ORI(CPUCore*, const N64Opcode& o, u32, u64* g) { g[o.rt] = u64m(g[o.rs] | o.imm); }
static void _h_ANDI(CPUCore*, const N64Opcode& o, u32, u64* g) { g[o.rt] = u64m(g[o.rs] & o.imm); }
static void _h_XORI(CPUCore*, const N64Opcode& o, u32, u64* g) { g[o.rt] = u64m(g[o.rs] ^ o.imm); }
// Integer overflow — Project64-Legacy DoIntegerOverflow (ExcCode 12)
static bool _ovf32_add(s32 a, s32 b, s32* out) {
  s32 r = (s32)((u32)a + (u32)b);
  *out = r;
  return ((a ^ r) & (b ^ r)) < 0;
}
static bool _ovf32_sub(s32 a, s32 b, s32* out) {
  s32 r = (s32)((u32)a - (u32)b);
  *out = r;
  return ((a ^ b) & (a ^ r)) < 0;
}
static bool _ovf64_add(s64 a, s64 b, s64* out) {
  s64 r = a + b; *out = r;
  return ((a ^ r) & (b ^ r)) < 0;
}
static bool _ovf64_sub(s64 a, s64 b, s64* out) {
  s64 r = a - b; *out = r;
  return ((a ^ b) & (a ^ r)) < 0;
}
static void _h_ADDI(CPUCore* cpu, const N64Opcode& o, u32 old_pc, u64* g) {
  s32 r; if (_ovf32_add(sign32(g[o.rs]), o.simm, &r)) { cpu->_raise_exception(EXC_OVERFLOW, old_pc); return; }
  g[o.rt] = sx32_to_64((u32)r);
}
static void _h_ADDIU(CPUCore*, const N64Opcode& o, u32, u64* g) { g[o.rt] = sx32_to_64(u32m(g[o.rs] + (u64)o.simm)); }
static void _h_DADDI(CPUCore* cpu, const N64Opcode& o, u32 old_pc, u64* g) {
  s64 r; if (_ovf64_add(sign64(g[o.rs]), (s64)o.simm, &r)) { cpu->_raise_exception(EXC_OVERFLOW, old_pc); return; }
  g[o.rt] = u64m((u64)r);
}
static void _h_DADDIU(CPUCore*, const N64Opcode& o, u32, u64* g) { g[o.rt] = u64m(g[o.rs] + (u64)(s64)o.simm); }
static void _h_SLTI(CPUCore*, const N64Opcode& o, u32, u64* g) { g[o.rt] = sign64(g[o.rs]) < o.simm ? 1 : 0; }
static void _h_SLTIU(CPUCore*, const N64Opcode& o, u32, u64* g) { g[o.rt] = g[o.rs] < u64m((u64)(s64)o.simm) ? 1 : 0; }
static void _h_LB(CPUCore* cpu, const N64Opcode& o, u32, u64* g) { g[o.rt] = sx8_to_64(cpu->core->bus.read_u8(u32m(g[o.rs] + (u64)o.simm))); }
static void _h_LBU(CPUCore* cpu, const N64Opcode& o, u32, u64* g) { g[o.rt] = cpu->core->bus.read_u8(u32m(g[o.rs] + (u64)o.simm)); }
static void _h_LH(CPUCore* cpu, const N64Opcode& o, u32, u64* g) { g[o.rt] = sx16_to_64(cpu->core->bus.read_u16(u32m(g[o.rs] + (u64)o.simm))); }
static void _h_LHU(CPUCore* cpu, const N64Opcode& o, u32, u64* g) { g[o.rt] = cpu->core->bus.read_u16(u32m(g[o.rs] + (u64)o.simm)); }
static void _h_LW(CPUCore* cpu, const N64Opcode& o, u32, u64* g) { g[o.rt] = sx32_to_64(cpu->core->bus.read_u32(u32m(g[o.rs] + (u64)o.simm))); }
static void _h_LWU(CPUCore* cpu, const N64Opcode& o, u32, u64* g) { g[o.rt] = cpu->core->bus.read_u32(u32m(g[o.rs] + (u64)o.simm)); }
static void _h_LD(CPUCore* cpu, const N64Opcode& o, u32, u64* g) { g[o.rt] = cpu->core->bus.read_u64(u32m(g[o.rs] + (u64)o.simm)); }
static void _h_LWL(CPUCore* cpu, const N64Opcode& o, u32, u64* g) {
  static const u32 mask[4]={0,0xFF,0xFFFF,0xFFFFFF}; static const int sh[4]={0,8,16,24};
  u32 addr=u32m(g[o.rs]+(u64)o.simm); u32 off=addr&3; u32 val=cpu->core->bus.read_u32(addr&~3u);
  g[o.rt]=sx32_to_64((u32m(g[o.rt])&mask[off])|(val<<sh[off]));
}
static void _h_LWR(CPUCore* cpu, const N64Opcode& o, u32, u64* g) {
  static const u32 mask[4]={0xFFFFFF00,0xFFFF0000,0xFF000000,0}; static const int sh[4]={24,16,8,0};
  u32 addr=u32m(g[o.rs]+(u64)o.simm); u32 off=addr&3; u32 val=cpu->core->bus.read_u32(addr&~3u);
  g[o.rt]=sx32_to_64((u32m(g[o.rt])&mask[off])|(val>>sh[off]));
}
static void _h_LDL(CPUCore* cpu, const N64Opcode& o, u32, u64* g) {
  static const u64 mask[8]={0,0xFF,0xFFFF,0xFFFFFF,0xFFFFFFFF,0xFFFFFFFFFF,0xFFFFFFFFFFFF,0xFFFFFFFFFFFFFF};
  static const int sh[8]={0,8,16,24,32,40,48,56};
  u32 addr=u32m(g[o.rs]+(u64)o.simm); u32 off=addr&7; u64 val=cpu->core->bus.read_u64(addr&~7u);
  g[o.rt]=(g[o.rt]&mask[off])|(val<<sh[off]);
}
static void _h_LDR(CPUCore* cpu, const N64Opcode& o, u32, u64* g) {
  static const u64 mask[8]={0xFFFFFFFFFFFFFF00ULL,0xFFFFFFFFFFFF0000ULL,0xFFFFFFFFFF000000ULL,0xFFFFFFFF00000000ULL,0xFFFFFF0000000000ULL,0xFFFF000000000000ULL,0xFF00000000000000ULL,0};
  static const int sh[8]={56,48,40,32,24,16,8,0};
  u32 addr=u32m(g[o.rs]+(u64)o.simm); u32 off=addr&7; u64 val=cpu->core->bus.read_u64(addr&~7u);
  g[o.rt]=(g[o.rt]&mask[off])|(val>>sh[off]);
}
static void _h_LL(CPUCore* cpu, const N64Opcode& o, u32, u64* g) {
  u32 addr=u32m(g[o.rs]+(u64)o.simm); g[o.rt]=sx32_to_64(cpu->core->bus.read_u32(addr));
  cpu->llbit=true; cpu->lladdr=addr&~3u;
}
static void _h_LLD(CPUCore* cpu, const N64Opcode& o, u32, u64* g) {
  u32 addr=u32m(g[o.rs]+(u64)o.simm); g[o.rt]=cpu->core->bus.read_u64(addr);
  cpu->llbit=true; cpu->lladdr=addr&~7u;
}
static void _h_LWC1(CPUCore* cpu, const N64Opcode& o, u32 old_pc, u64* g) {
  if (!cpu->_test_cop1_usable()) { cpu->_raise_exception(11, old_pc, false, 1); return; }
  cpu->fset32(o.rt, cpu->core->bus.read_u32(u32m(g[o.rs]+(u64)o.simm)));
}
static void _h_LDC1(CPUCore* cpu, const N64Opcode& o, u32 old_pc, u64* g) {
  if (!cpu->_test_cop1_usable()) { cpu->_raise_exception(11, old_pc, false, 1); return; }
  cpu->fset64(o.rt, cpu->core->bus.read_u64(u32m(g[o.rs]+(u64)o.simm)));
}
static void _h_SB(CPUCore* cpu, const N64Opcode& o, u32, u64* g) { cpu->core->bus.write_u8(u32m(g[o.rs]+(u64)o.simm), u8m(g[o.rt])); }
static void _h_SH(CPUCore* cpu, const N64Opcode& o, u32, u64* g) { cpu->core->bus.write_u16(u32m(g[o.rs]+(u64)o.simm), u16m(g[o.rt])); }
static void _h_SW(CPUCore* cpu, const N64Opcode& o, u32, u64* g) { cpu->core->bus.write_u32(u32m(g[o.rs]+(u64)o.simm), u32m(g[o.rt])); }
static void _h_SD(CPUCore* cpu, const N64Opcode& o, u32, u64* g) { cpu->core->bus.write_u64(u32m(g[o.rs]+(u64)o.simm), g[o.rt]); }
static void _h_SWL(CPUCore* cpu, const N64Opcode& o, u32, u64* g) {
  static const u32 mask[4]={0,0xFF000000,0xFFFF0000,0xFFFFFF00}; static const int sh[4]={0,8,16,24};
  u32 addr=u32m(g[o.rs]+(u64)o.simm); u32 off=addr&3; u32 al=addr&~3u;
  u32 val=cpu->core->bus.read_u32(al);
  cpu->core->bus.write_u32(al, (val&mask[off])|(u32m(g[o.rt])>>sh[off]));
}
static void _h_SWR(CPUCore* cpu, const N64Opcode& o, u32, u64* g) {
  static const u32 mask[4]={0x00FFFFFF,0x0000FFFF,0x000000FF,0}; static const int sh[4]={24,16,8,0};
  u32 addr=u32m(g[o.rs]+(u64)o.simm); u32 off=addr&3; u32 al=addr&~3u;
  u32 val=cpu->core->bus.read_u32(al);
  cpu->core->bus.write_u32(al, (val&mask[off])|(u32m(g[o.rt])<<sh[off]));
}
static void _h_SDL(CPUCore* cpu, const N64Opcode& o, u32, u64* g) {
  static const u64 mask[8]={0,0xFF00000000000000ULL,0xFFFF000000000000ULL,0xFFFFFF0000000000ULL,0xFFFFFFFF00000000ULL,0xFFFFFFFFFF000000ULL,0xFFFFFFFFFFFF0000ULL,0xFFFFFFFFFFFFFF00ULL};
  static const int sh[8]={0,8,16,24,32,40,48,56};
  u32 addr=u32m(g[o.rs]+(u64)o.simm); u32 off=addr&7; u32 al=addr&~7u;
  u64 val=cpu->core->bus.read_u64(al);
  cpu->core->bus.write_u64(al, (val&mask[off])|(g[o.rt]>>sh[off]));
}
static void _h_SDR(CPUCore* cpu, const N64Opcode& o, u32, u64* g) {
  static const u64 mask[8]={0x00FFFFFFFFFFFFFFULL,0x0000FFFFFFFFFFFFULL,0x000000FFFFFFFFFFULL,0x00000000FFFFFFFFULL,0x0000000000FFFFFFULL,0x000000000000FFFFULL,0x00000000000000FFULL,0};
  static const int sh[8]={56,48,40,32,24,16,8,0};
  u32 addr=u32m(g[o.rs]+(u64)o.simm); u32 off=addr&7; u32 al=addr&~7u;
  u64 val=cpu->core->bus.read_u64(al);
  cpu->core->bus.write_u64(al, (val&mask[off])|(g[o.rt]<<sh[off]));
}
static void _h_SC(CPUCore* cpu, const N64Opcode& o, u32, u64* g) {
  u32 addr=u32m(g[o.rs]+(u64)o.simm);
  if (cpu->llbit && (addr&~3u)==cpu->lladdr) { cpu->core->bus.write_u32(addr, u32m(g[o.rt])); g[o.rt]=1; }
  else g[o.rt]=0;
  cpu->llbit=false;
}
static void _h_SCD(CPUCore* cpu, const N64Opcode& o, u32, u64* g) {
  u32 addr=u32m(g[o.rs]+(u64)o.simm);
  if (cpu->llbit && (addr&~7u)==cpu->lladdr) { cpu->core->bus.write_u64(addr, g[o.rt]); g[o.rt]=1; }
  else g[o.rt]=0;
  cpu->llbit=false;
}
static void _h_SWC1(CPUCore* cpu, const N64Opcode& o, u32 old_pc, u64* g) {
  if (!cpu->_test_cop1_usable()) { cpu->_raise_exception(11, old_pc, false, 1); return; }
  cpu->core->bus.write_u32(u32m(g[o.rs]+(u64)o.simm), cpu->fget32(o.rt));
}
static void _h_SDC1(CPUCore* cpu, const N64Opcode& o, u32 old_pc, u64* g) {
  if (!cpu->_test_cop1_usable()) { cpu->_raise_exception(11, old_pc, false, 1); return; }
  cpu->core->bus.write_u64(u32m(g[o.rs]+(u64)o.simm), cpu->fget64(o.rt));
}
static void _h_SLL(CPUCore*, const N64Opcode& o, u32, u64* g) { g[o.rd]=sx32_to_64(u32m(g[o.rt])<<o.sa); }
static void _h_SRL(CPUCore*, const N64Opcode& o, u32, u64* g) { g[o.rd]=sx32_to_64(u32m(g[o.rt])>>o.sa); }
static void _h_SRA(CPUCore*, const N64Opcode& o, u32, u64* g) { g[o.rd]=sx32_to_64((u32)(sign32(g[o.rt])>>o.sa)); }
static void _h_SLLV(CPUCore*, const N64Opcode& o, u32, u64* g) { g[o.rd]=sx32_to_64(u32m(g[o.rt])<<(g[o.rs]&0x1F)); }
static void _h_SRLV(CPUCore*, const N64Opcode& o, u32, u64* g) { g[o.rd]=sx32_to_64(u32m(g[o.rt])>>(g[o.rs]&0x1F)); }
static void _h_SRAV(CPUCore*, const N64Opcode& o, u32, u64* g) { g[o.rd]=sx32_to_64((u32)(sign32(g[o.rt])>>(g[o.rs]&0x1F))); }
static void _h_DSLLV(CPUCore*, const N64Opcode& o, u32, u64* g) { g[o.rd]=u64m(g[o.rt]<<(g[o.rs]&0x3F)); }
static void _h_DSRLV(CPUCore*, const N64Opcode& o, u32, u64* g) { g[o.rd]=u64m(g[o.rt]>>(g[o.rs]&0x3F)); }
static void _h_DSRAV(CPUCore*, const N64Opcode& o, u32, u64* g) { g[o.rd]=u64m((u64)(sign64(g[o.rt])>>(g[o.rs]&0x3F))); }
static void _h_DSLL(CPUCore*, const N64Opcode& o, u32, u64* g) { g[o.rd]=u64m(g[o.rt]<<o.sa); }
static void _h_DSRL(CPUCore*, const N64Opcode& o, u32, u64* g) { g[o.rd]=u64m(g[o.rt]>>o.sa); }
static void _h_DSRA(CPUCore*, const N64Opcode& o, u32, u64* g) { g[o.rd]=u64m((u64)(sign64(g[o.rt])>>o.sa)); }
static void _h_DSLL32(CPUCore*, const N64Opcode& o, u32, u64* g) { g[o.rd]=u64m(g[o.rt]<<(o.sa+32)); }
static void _h_DSRL32(CPUCore*, const N64Opcode& o, u32, u64* g) { g[o.rd]=u64m(g[o.rt]>>(o.sa+32)); }
static void _h_DSRA32(CPUCore*, const N64Opcode& o, u32, u64* g) { g[o.rd]=u64m((u64)(sign64(g[o.rt])>>(o.sa+32))); }
static void _h_ADD(CPUCore* cpu, const N64Opcode& o, u32 old_pc, u64* g) {
  s32 r; if (_ovf32_add(sign32(g[o.rs]), sign32(g[o.rt]), &r)) { cpu->_raise_exception(EXC_OVERFLOW, old_pc); return; }
  g[o.rd] = sx32_to_64((u32)r);
}
static void _h_ADDU(CPUCore*, const N64Opcode& o, u32, u64* g) { g[o.rd]=sx32_to_64(u32m(g[o.rs]+g[o.rt])); }
static void _h_SUB(CPUCore* cpu, const N64Opcode& o, u32 old_pc, u64* g) {
  s32 r; if (_ovf32_sub(sign32(g[o.rs]), sign32(g[o.rt]), &r)) { cpu->_raise_exception(EXC_OVERFLOW, old_pc); return; }
  g[o.rd] = sx32_to_64((u32)r);
}
static void _h_SUBU(CPUCore*, const N64Opcode& o, u32, u64* g) { g[o.rd]=sx32_to_64(u32m(g[o.rs]-g[o.rt])); }
static void _h_DADD(CPUCore* cpu, const N64Opcode& o, u32 old_pc, u64* g) {
  s64 r; if (_ovf64_add(sign64(g[o.rs]), sign64(g[o.rt]), &r)) { cpu->_raise_exception(EXC_OVERFLOW, old_pc); return; }
  g[o.rd] = u64m((u64)r);
}
static void _h_DADDU(CPUCore*, const N64Opcode& o, u32, u64* g) { g[o.rd]=u64m(g[o.rs]+g[o.rt]); }
static void _h_DSUB(CPUCore* cpu, const N64Opcode& o, u32 old_pc, u64* g) {
  s64 r; if (_ovf64_sub(sign64(g[o.rs]), sign64(g[o.rt]), &r)) { cpu->_raise_exception(EXC_OVERFLOW, old_pc); return; }
  g[o.rd] = u64m((u64)r);
}
static void _h_DSUBU(CPUCore*, const N64Opcode& o, u32, u64* g) { g[o.rd]=u64m(g[o.rs]-g[o.rt]); }
static void _h_AND(CPUCore*, const N64Opcode& o, u32, u64* g) { g[o.rd]=u64m(g[o.rs]&g[o.rt]); }
static void _h_OR(CPUCore*, const N64Opcode& o, u32, u64* g) { g[o.rd]=u64m(g[o.rs]|g[o.rt]); }
static void _h_XOR(CPUCore*, const N64Opcode& o, u32, u64* g) { g[o.rd]=u64m(g[o.rs]^g[o.rt]); }
static void _h_NOR(CPUCore*, const N64Opcode& o, u32, u64* g) { g[o.rd]=u64m(~(g[o.rs]|g[o.rt])); }
static void _h_MOVZ(CPUCore*, const N64Opcode& o, u32, u64* g) { if (g[o.rt]==0) g[o.rd]=g[o.rs]; }
static void _h_MOVN(CPUCore*, const N64Opcode& o, u32, u64* g) { if (g[o.rt]!=0) g[o.rd]=g[o.rs]; }
static void _h_SLT(CPUCore*, const N64Opcode& o, u32, u64* g) { g[o.rd]=sign64(g[o.rs])<sign64(g[o.rt])?1:0; }
static void _h_SLTU(CPUCore*, const N64Opcode& o, u32, u64* g) { g[o.rd]=g[o.rs]<g[o.rt]?1:0; }
static void _h_MFHI(CPUCore* cpu, const N64Opcode& o, u32, u64* g) { g[o.rd]=cpu->hi; }
static void _h_MTHI(CPUCore* cpu, const N64Opcode& o, u32, u64* g) { cpu->hi=u64m(g[o.rs]); }
static void _h_MFLO(CPUCore* cpu, const N64Opcode& o, u32, u64* g) { g[o.rd]=cpu->lo; }
static void _h_MTLO(CPUCore* cpu, const N64Opcode& o, u32, u64* g) { cpu->lo=u64m(g[o.rs]); }
static void _h_MULT(CPUCore* cpu, const N64Opcode& o, u32, u64* g) {
  s64 r=(s64)sign32(g[o.rs])*(s64)sign32(g[o.rt]); cpu->lo=sx32_to_64((u32)r); cpu->hi=sx32_to_64((u32)(r>>32));
}
static void _h_MULTU(CPUCore* cpu, const N64Opcode& o, u32, u64* g) {
  u64 r=(u64)u32m(g[o.rs])*(u64)u32m(g[o.rt]); cpu->lo=sx32_to_64((u32)r); cpu->hi=sx32_to_64((u32)(r>>32));
}
static void _h_DMULT(CPUCore* cpu, const N64Opcode& o, u32, u64* g) {
  // signed 64*64 -> 128 truncated to lo/hi like Python __int128 path simplified
  __int128 r=(__int128)sign64(g[o.rs])*(__int128)sign64(g[o.rt]);
  cpu->lo=(u64)r; cpu->hi=(u64)(r>>64);
}
static void _h_DMULTU(CPUCore* cpu, const N64Opcode& o, u32, u64* g) {
  __uint128_t r=(__uint128_t)g[o.rs]*(__uint128_t)g[o.rt];
  cpu->lo=(u64)r; cpu->hi=(u64)(r>>64);
}
static void _h_DIV(CPUCore* cpu, const N64Opcode& o, u32, u64* g) {
  s32 a = sign32(g[o.rs]), b = sign32(g[o.rt]);
  if (b == 0) {
    // MIPS: lo = -1 if dividend >= 0 else +1; hi = dividend
    cpu->lo = a < 0 ? 1 : MASK_64;
    cpu->hi = sx32_to_64((u32)a);
  } else if (a == (s32)0x80000000 && b == -1) {
    cpu->lo = sx32_to_64(0x80000000u); cpu->hi = 0;
  } else {
    s32 q = a / b, r = a % b;
    cpu->lo = sx32_to_64((u32)q); cpu->hi = sx32_to_64((u32)r);
  }
}
static void _h_DIVU(CPUCore* cpu, const N64Opcode& o, u32, u64* g) {
  u32 a = u32m(g[o.rs]), b = u32m(g[o.rt]);
  if (b == 0) { cpu->lo = MASK_64; cpu->hi = sx32_to_64(a); }
  else { cpu->lo = sx32_to_64(a / b); cpu->hi = sx32_to_64(a % b); }
}
static void _h_DDIV(CPUCore* cpu, const N64Opcode& o, u32, u64* g) {
  s64 a = sign64(g[o.rs]), b = sign64(g[o.rt]);
  if (b == 0) {
    cpu->lo = a < 0 ? 1 : MASK_64;
    cpu->hi = u64m((u64)a);
  } else if (a == (s64)0x8000000000000000LL && b == -1) {
    cpu->lo = 0x8000000000000000ULL; cpu->hi = 0;
  } else {
    cpu->lo = u64m((u64)(a / b)); cpu->hi = u64m((u64)(a % b));
  }
}
static void _h_DDIVU(CPUCore* cpu, const N64Opcode& o, u32, u64* g) {
  u64 a = g[o.rs], b = g[o.rt];
  if (b == 0) { cpu->lo = MASK_64; cpu->hi = a; }
  else { cpu->lo = a / b; cpu->hi = a % b; }
}
static void _h_JR(CPUCore* cpu, const N64Opcode& o, u32, u64* g) { cpu->_branch(u32m(g[o.rs])); }
static void _h_JALR(CPUCore* cpu, const N64Opcode& o, u32 old_pc, u64* g) { g[o.rd]=u64m(old_pc+8); cpu->_branch(u32m(g[o.rs])); }
static void _h_J(CPUCore* cpu, const N64Opcode& o, u32 old_pc, u64*) { cpu->_branch(o.target_addr(old_pc)); }
static void _h_JAL(CPUCore* cpu, const N64Opcode& o, u32 old_pc, u64* g) { g[31]=u64m(old_pc+8); cpu->_branch(o.target_addr(old_pc)); }
static void _h_BEQ(CPUCore* cpu, const N64Opcode& o, u32 old_pc, u64* g) { if (g[o.rs]==g[o.rt]) cpu->_branch(o.branch_addr(old_pc)); }
static void _h_BNE(CPUCore* cpu, const N64Opcode& o, u32 old_pc, u64* g) { if (g[o.rs]!=g[o.rt]) cpu->_branch(o.branch_addr(old_pc)); }
static void _h_BLEZ(CPUCore* cpu, const N64Opcode& o, u32 old_pc, u64* g) { if (sign64(g[o.rs])<=0) cpu->_branch(o.branch_addr(old_pc)); }
static void _h_BGTZ(CPUCore* cpu, const N64Opcode& o, u32 old_pc, u64* g) { if (sign64(g[o.rs])>0) cpu->_branch(o.branch_addr(old_pc)); }
static void _h_BEQL(CPUCore* cpu, const N64Opcode& o, u32 old_pc, u64* g) { if (g[o.rs]==g[o.rt]) cpu->_branch(o.branch_addr(old_pc)); else cpu->_skip_likely(); }
static void _h_BNEL(CPUCore* cpu, const N64Opcode& o, u32 old_pc, u64* g) { if (g[o.rs]!=g[o.rt]) cpu->_branch(o.branch_addr(old_pc)); else cpu->_skip_likely(); }
static void _h_BLEZL(CPUCore* cpu, const N64Opcode& o, u32 old_pc, u64* g) { if (sign64(g[o.rs])<=0) cpu->_branch(o.branch_addr(old_pc)); else cpu->_skip_likely(); }
static void _h_BGTZL(CPUCore* cpu, const N64Opcode& o, u32 old_pc, u64* g) { if (sign64(g[o.rs])>0) cpu->_branch(o.branch_addr(old_pc)); else cpu->_skip_likely(); }
static void _h_BLTZ(CPUCore* cpu, const N64Opcode& o, u32 old_pc, u64* g) { if (sign64(g[o.rs])<0) cpu->_branch(o.branch_addr(old_pc)); }
static void _h_BGEZ(CPUCore* cpu, const N64Opcode& o, u32 old_pc, u64* g) { if (sign64(g[o.rs])>=0) cpu->_branch(o.branch_addr(old_pc)); }
static void _h_BLTZL(CPUCore* cpu, const N64Opcode& o, u32 old_pc, u64* g) { if (sign64(g[o.rs])<0) cpu->_branch(o.branch_addr(old_pc)); else cpu->_skip_likely(); }
static void _h_BGEZL(CPUCore* cpu, const N64Opcode& o, u32 old_pc, u64* g) { if (sign64(g[o.rs])>=0) cpu->_branch(o.branch_addr(old_pc)); else cpu->_skip_likely(); }
static void _h_BLTZAL(CPUCore* cpu, const N64Opcode& o, u32 old_pc, u64* g) { g[31]=u64m(old_pc+8); if (sign64(g[o.rs])<0) cpu->_branch(o.branch_addr(old_pc)); }
static void _h_BGEZAL(CPUCore* cpu, const N64Opcode& o, u32 old_pc, u64* g) { g[31]=u64m(old_pc+8); if (sign64(g[o.rs])>=0) cpu->_branch(o.branch_addr(old_pc)); }
static void _h_BLTZALL(CPUCore* cpu, const N64Opcode& o, u32 old_pc, u64* g) { g[31]=u64m(old_pc+8); if (sign64(g[o.rs])<0) cpu->_branch(o.branch_addr(old_pc)); else cpu->_skip_likely(); }
static void _h_BGEZALL(CPUCore* cpu, const N64Opcode& o, u32 old_pc, u64* g) { g[31]=u64m(old_pc+8); if (sign64(g[o.rs])>=0) cpu->_branch(o.branch_addr(old_pc)); else cpu->_skip_likely(); }
static void _h_MFC0(CPUCore* cpu, const N64Opcode& o, u32, u64* g) { g[o.rt]=sx32_to_64(cpu->cp0[o.rd]); }
static void _h_DMFC0(CPUCore* cpu, const N64Opcode& o, u32, u64* g) { g[o.rt]=u64m(cpu->cp0[o.rd]); }
static void _h_CFC0(CPUCore* cpu, const N64Opcode& o, u32, u64* g) { g[o.rt]=sx32_to_64(cpu->cp0[o.rd]); }
static void _h_MTC0(CPUCore* cpu, const N64Opcode& o, u32, u64* g) {
  u32 val=u32m(g[o.rt]); int rd=o.rd;
  if (rd==CP0_INDEX) cpu->cp0[rd]=val&0x8000003F;
  else if (rd==CP0_ENTRYLO0||rd==CP0_ENTRYLO1) cpu->cp0[rd]=val&0x3FFFFFFF;
  else if (rd==CP0_PAGEMASK) cpu->cp0[rd]=val&0x01FFE000;
  else if (rd==CP0_WIRED) { cpu->cp0[rd]=val&0x3F; cpu->cp0[CP0_RANDOM]=31; }
  else if (rd==CP0_CONTEXT) cpu->cp0[rd]=(cpu->cp0[rd]&0x7FFFFF)|(val&0xFF800000);
  else if (rd==CP0_COUNT) cpu->cp0[rd]=val;
  else if (rd==CP0_COMPARE) { cpu->cp0[rd]=val; cpu->cp0[CP0_CAUSE]&=~CAUSE_IP7; }
  else if (rd==CP0_CAUSE) cpu->cp0[rd]=(cpu->cp0[rd]&~0x300)|(val&0x300);
  else if (rd==CP0_ENTRYHI) cpu->cp0[rd]=val&0xFFFFE0FF;
  else if (rd==CP0_STATUS||rd==CP0_EPC||rd==CP0_ERROREPC||rd==CP0_LLADDR||rd==CP0_CONFIG||rd==28||rd==29) cpu->cp0[rd]=val;
}
static void _h_DMTC0(CPUCore* cpu, const N64Opcode& o, u32, u64* g) { cpu->cp0[o.rd]=u32m(g[o.rt]); }
static void _h_CTC0(CPUCore* cpu, const N64Opcode& o, u32, u64* g) { cpu->cp0[o.rd]=u32m(g[o.rt]); }
static void _h_BC0(CPUCore* cpu, const N64Opcode& o, u32 old_pc, u64*) {
  bool tf=o.rt&1, likely=o.rt&2; bool cond=false;
  if (cond==tf) cpu->_branch(o.branch_addr(old_pc)); else if (likely) cpu->_skip_likely();
}
static void _h_ERET(CPUCore* cpu, const N64Opcode&, u32, u64*) {
  u32 target;
  if (cpu->cp0[CP0_STATUS]&STATUS_ERL) { target=cpu->cp0[CP0_ERROREPC]; cpu->cp0[CP0_STATUS]&=~STATUS_ERL; }
  else { target=cpu->cp0[CP0_EPC]; cpu->cp0[CP0_STATUS]&=~STATUS_EXL; }
  cpu->pc=u32m(target); cpu->next_pc=u32m(cpu->pc+4);
}
static void _h_TLBWI(CPUCore* cpu, const N64Opcode&, u32, u64*) { cpu->_write_tlb_entry(cpu->cp0[CP0_INDEX]&0x1F); }
static void _h_TLBWR(CPUCore* cpu, const N64Opcode&, u32, u64*) { int w=cpu->cp0[CP0_WIRED]&0x1F; cpu->_write_tlb_entry(w+((int)(cpu->cp0[CP0_COUNT])%(32-w))); }
static void _h_TLBP(CPUCore* cpu, const N64Opcode&, u32, u64*) {
  u32 hi=cpu->cp0[CP0_ENTRYHI]; u32 vpn2=(hi>>13)&0x7FFFF; u32 asid=hi&0xFF; int match=-1;
  for (int i=0;i<32;i++) {
    u32 mask=(cpu->tlb[i].mask>>13)&0xFFF;
    if ((cpu->tlb[i].vpn2&~mask)==(vpn2&~mask) && (cpu->tlb[i].g||cpu->tlb[i].asid==asid)) { match=i; break; }
  }
  cpu->cp0[CP0_INDEX]=match>=0?(u32)match:0x80000000u;
}
static void _h_TLBR(CPUCore* cpu, const N64Opcode&, u32, u64*) {
  int idx=cpu->cp0[CP0_INDEX]&0x1F; auto& e=cpu->tlb[idx];
  cpu->cp0[CP0_PAGEMASK]=e.mask; cpu->cp0[CP0_ENTRYHI]=(e.vpn2<<13)|e.asid;
  cpu->cp0[CP0_ENTRYLO0]=(e.pfn0<<6)|(e.c0<<3)|(e.d0<<2)|(e.v0<<1)|e.g;
  cpu->cp0[CP0_ENTRYLO1]=(e.pfn1<<6)|(e.c1<<3)|(e.d1<<2)|(e.v1<<1)|e.g;
}
static void _h_MFC1(CPUCore* cpu, const N64Opcode& o, u32 old_pc, u64* g) {
  if (!cpu->_test_cop1_usable()) { cpu->_raise_exception(11, old_pc, false, 1); return; }
  g[o.rt]=sx32_to_64(cpu->fget32(o.rd));
}
static void _h_DMFC1(CPUCore* cpu, const N64Opcode& o, u32 old_pc, u64* g) {
  if (!cpu->_test_cop1_usable()) { cpu->_raise_exception(11, old_pc, false, 1); return; }
  g[o.rt]=cpu->fget64(o.rd);
}
static void _h_CFC1(CPUCore* cpu, const N64Opcode& o, u32 old_pc, u64* g) {
  if (!cpu->_test_cop1_usable()) { cpu->_raise_exception(11, old_pc, false, 1); return; }
  g[o.rt]=sx32_to_64(o.rd==31?cpu->fcr31:cpu->fcr0);
}
// Project64-Legacy r4300i_COP1_DCF / DCT — unimplemented FP op
static void _h_DCFC1(CPUCore* cpu, const N64Opcode&, u32 old_pc, u64*) {
  if (!cpu->_test_cop1_usable()) { cpu->_raise_exception(11, old_pc, false, 1); return; }
  cpu->_clear_fp_cause(); cpu->_set_fp_cause(FCR31_CAUSE_UNIMP);
  if (cpu->_fp_exception_enabled(FCR31_CAUSE_UNIMP)) cpu->_raise_exception(EXC_FPE, old_pc);
}
static void _h_MTC1(CPUCore* cpu, const N64Opcode& o, u32 old_pc, u64* g) {
  if (!cpu->_test_cop1_usable()) { cpu->_raise_exception(11, old_pc, false, 1); return; }
  cpu->fset32(o.rd, u32m(g[o.rt]));
}
static void _h_DMTC1(CPUCore* cpu, const N64Opcode& o, u32 old_pc, u64* g) {
  if (!cpu->_test_cop1_usable()) { cpu->_raise_exception(11, old_pc, false, 1); return; }
  cpu->fset64(o.rd, g[o.rt]);
}
static void _h_CTC1(CPUCore* cpu, const N64Opcode& o, u32 old_pc, u64* g) {
  if (!cpu->_test_cop1_usable()) { cpu->_raise_exception(11, old_pc, false, 1); return; }
  if (o.rd==31) cpu->fcr31=u32m(g[o.rt]); else if (o.rd==0) cpu->fcr0=u32m(g[o.rt]);
}
static void _h_DCTC1(CPUCore* cpu, const N64Opcode&, u32 old_pc, u64*) {
  if (!cpu->_test_cop1_usable()) { cpu->_raise_exception(11, old_pc, false, 1); return; }
  cpu->_clear_fp_cause(); cpu->_set_fp_cause(FCR31_CAUSE_UNIMP);
  if (cpu->_fp_exception_enabled(FCR31_CAUSE_UNIMP)) cpu->_raise_exception(EXC_FPE, old_pc);
}
// Project64-Legacy COP2 (cop2LatchedValue) — Interpreter Ops.c
static void _h_COP2(CPUCore* cpu, const N64Opcode& o, u32 old_pc, u64* g) {
  if (!cpu->_test_cop2_usable()) { cpu->_raise_exception(11, old_pc, false, 2); return; }
  switch (o.rs) {
    case 0: g[o.rt] = sx32_to_64((u32)cpu->cop2_latch); break; // MFC2
    case 1: g[o.rt] = cpu->cop2_latch; break; // DMFC2
    case 2: break; // CFC2 — nop when usable (Legacy)
    case 3: // DCFC2 — illegal
    case 7: // DCTC2 — illegal
      cpu->_raise_exception(10, old_pc); break;
    case 4: case 5: cpu->cop2_latch = g[o.rt]; break; // MTC2 / DMTC2
    case 6: break; // CTC2 — nop
    default: cpu->_raise_exception(10, old_pc); break;
  }
}
static void _h_BC1(CPUCore* cpu, const N64Opcode& o, u32 old_pc, u64*) {
  if (!cpu->_test_cop1_usable()) { cpu->_raise_exception(11, old_pc, false, 1); return; }
  bool tf=o.rt&1, likely=o.rt&2;
  bool cond=((cpu->fcr31>>FCR31_COND_BIT)&1)!=0;
  if (cond==tf) cpu->_branch(o.branch_addr(old_pc)); else if (likely) cpu->_skip_likely();
}
static void _h_SYSCALL(CPUCore* cpu, const N64Opcode&, u32 old_pc, u64*) { cpu->_raise_exception(8, old_pc); }
static void _h_BREAK(CPUCore* cpu, const N64Opcode&, u32 old_pc, u64*) { cpu->_raise_exception(9, old_pc); }
static void _h_TGE(CPUCore* cpu, const N64Opcode& o, u32 old_pc, u64* g) { if (sign64(g[o.rs])>=sign64(g[o.rt])) cpu->_raise_exception(13, old_pc); }
static void _h_TGEU(CPUCore* cpu, const N64Opcode& o, u32 old_pc, u64* g) { if (g[o.rs]>=g[o.rt]) cpu->_raise_exception(13, old_pc); }
static void _h_TLT(CPUCore* cpu, const N64Opcode& o, u32 old_pc, u64* g) { if (sign64(g[o.rs])<sign64(g[o.rt])) cpu->_raise_exception(13, old_pc); }
static void _h_TLTU(CPUCore* cpu, const N64Opcode& o, u32 old_pc, u64* g) { if (g[o.rs]<g[o.rt]) cpu->_raise_exception(13, old_pc); }
static void _h_TEQ(CPUCore* cpu, const N64Opcode& o, u32 old_pc, u64* g) { if (g[o.rs]==g[o.rt]) cpu->_raise_exception(13, old_pc); }
static void _h_TNE(CPUCore* cpu, const N64Opcode& o, u32 old_pc, u64* g) { if (g[o.rs]!=g[o.rt]) cpu->_raise_exception(13, old_pc); }
static void _h_TGEI(CPUCore* cpu, const N64Opcode& o, u32 old_pc, u64* g) { if (sign64(g[o.rs])>=o.simm) cpu->_raise_exception(13, old_pc); }
static void _h_TGEIU(CPUCore* cpu, const N64Opcode& o, u32 old_pc, u64* g) { if (g[o.rs]>=u64m((u64)(s64)o.simm)) cpu->_raise_exception(13, old_pc); }
static void _h_TLTI(CPUCore* cpu, const N64Opcode& o, u32 old_pc, u64* g) { if (sign64(g[o.rs])<o.simm) cpu->_raise_exception(13, old_pc); }
static void _h_TLTIU(CPUCore* cpu, const N64Opcode& o, u32 old_pc, u64* g) { if (g[o.rs]<u64m((u64)(s64)o.simm)) cpu->_raise_exception(13, old_pc); }
static void _h_TEQI(CPUCore* cpu, const N64Opcode& o, u32 old_pc, u64* g) { if (g[o.rs]==u64m((u64)(s64)o.simm)) cpu->_raise_exception(13, old_pc); }
static void _h_TNEI(CPUCore* cpu, const N64Opcode& o, u32 old_pc, u64* g) { if (g[o.rs]!=u64m((u64)(s64)o.simm)) cpu->_raise_exception(13, old_pc); }
static void _h_CACHE(CPUCore*, const N64Opcode&, u32, u64*) {}
static void _h_SYNC(CPUCore*, const N64Opcode&, u32, u64*) {}
static void _h_WAIT(CPUCore*, const N64Opcode&, u32, u64*) {}
static void _h_RI(CPUCore* cpu, const N64Opcode&, u32 old_pc, u64*) { cpu->_raise_exception(10, old_pc); }
static void _h_CPU_UNUSABLE(CPUCore* cpu, const N64Opcode& o, u32 old_pc, u64*) { cpu->_raise_exception(11, old_pc, false, o.op&3); }
static void _h_GROUP(CPUCore*, const N64Opcode&, u32, u64*) {}

static bool _fp_cmp(double a, double b, int funct) {
  bool nan = std::isnan(a) || std::isnan(b); int f = funct & 7;
  if (f==0) return false; if (f==1) return nan; if (f==2) return !nan && a==b; if (f==3) return nan||a==b;
  if (f==4) return !nan && a<b; if (f==5) return nan||a<b; if (f==6) return !nan && a<=b; return nan||a<=b;
}
static u64 _fp_to_int(double f, int mode, int bits) {
  if (std::isnan(f) || std::isinf(f)) return (1ULL<<(bits-1))-1;
  double v;
  if (mode==0) v=std::nearbyint(f); else if (mode==1) v=(double)(s64)f; else if (mode==2) v=std::ceil(f); else v=std::floor(f);
  s64 lim = 1LL<<(bits-1);
  if (v >= (double)lim || v < (double)(-lim)) return (u64)(lim-1);
  return (u64)(s64)v & ((1ULL<<bits)-1);
}
static void _h_FPU(CPUCore* cpu, const N64Opcode& o, u32 old_pc, u64*) {
  if (!cpu->_test_cop1_usable()) { cpu->_raise_exception(11, old_pc, false, 1); return; }
  cpu->_clear_fp_cause();
  int fmt_id=(o.instr_id>>6)&3, funct=o.instr_id&0x3F;
  int fs=o.rd, fd=o.sa, ft=o.rt; int rm=cpu->fcr31&3;
  if (fmt_id==_ID_FPU_S || fmt_id==_ID_FPU_D) {
    bool dbl=fmt_id==_ID_FPU_D;
    double a=dbl?bits_to_f64(cpu->fget64(fs)):bits_to_f32(cpu->fget32(fs));
    double b=dbl?bits_to_f64(cpu->fget64(ft)):bits_to_f32(cpu->fget32(ft));
    auto put=[&](double v){ if(dbl) cpu->fset64(fd,f64_to_bits(v)); else cpu->fset32(fd,f32_to_bits((float)v)); };
    if (funct==0x00) put(a+b); else if (funct==0x01) put(a-b); else if (funct==0x02) put(a*b);
    else if (funct==0x03) {
      if (b==0.0) { cpu->_set_fp_cause(FCR31_CAUSE_DIVBYZERO); cpu->_set_fp_flags(FCR31_CAUSE_DIVBYZERO);
        put((a==0.0||std::isnan(a))?NAN:std::copysign(INFINITY,a)*std::copysign(1.0,b)); }
      else put(a/b);
    } else if (funct==0x04) put(a>=0?std::sqrt(a):NAN);
    else if (funct==0x05) put(std::fabs(a));
    else if (funct==0x06) { if(dbl) cpu->fset64(fd,cpu->fget64(fs)); else cpu->fset32(fd,cpu->fget32(fs)); }
    else if (funct==0x07) put(-a);
    else if (funct==0x15) {
      if (a==0.0) { cpu->_set_fp_cause(FCR31_CAUSE_DIVBYZERO); cpu->_set_fp_flags(FCR31_CAUSE_DIVBYZERO); }
      put(a!=0.0?1.0/a:INFINITY);
    } else if (funct==0x16) {
      if (a<0) { cpu->_set_fp_cause(FCR31_CAUSE_INVALID); put(NAN); }
      else if (a==0.0) { cpu->_set_fp_cause(FCR31_CAUSE_DIVBYZERO); put(INFINITY); }
      else put(1.0/std::sqrt(a));
    } else if (funct==0x20) cpu->fset32(fd, f32_to_bits((float)a));
    else if (funct==0x21) cpu->fset64(fd, f64_to_bits(a));
    else if (funct==0x24) cpu->fset32(fd, (u32)_fp_to_int(a, rm, 32));
    else if (funct==0x25) cpu->fset64(fd, _fp_to_int(a, rm, 64));
    else if (funct>=0x0C && funct<=0x0F) cpu->fset32(fd, (u32)_fp_to_int(a, funct-0x0C, 32));
    else if (funct>=0x08 && funct<=0x0B) cpu->fset64(fd, _fp_to_int(a, funct-0x08, 64));
    else if (funct>=0x30) {
      if (_fp_cmp(a,b,funct)) cpu->fcr31 |= (1u<<FCR31_COND_BIT); else cpu->fcr31 &= ~(1u<<FCR31_COND_BIT);
    }
  } else if (fmt_id==_ID_FPU_W) {
    // Legacy COP1.W: only CVT.S / CVT.D implemented; rest → UNIMP (Interpreter Ops.c)
    s32 wi=sign32(cpu->fget32(fs));
    if (funct==0x20) cpu->fset32(fd, f32_to_bits((float)wi));
    else if (funct==0x21) cpu->fset64(fd, f64_to_bits((double)wi));
    else {
      cpu->_set_fp_cause(FCR31_CAUSE_UNIMP);
      if (cpu->_fp_exception_enabled(FCR31_CAUSE_UNIMP)) cpu->_raise_exception(EXC_FPE, old_pc);
    }
  } else if (fmt_id==_ID_FPU_L) {
    s64 li=sign64(cpu->fget64(fs));
    if (funct==0x20) cpu->fset32(fd, f32_to_bits((float)li));
    else if (funct==0x21) cpu->fset64(fd, f64_to_bits((double)li));
    else {
      cpu->_set_fp_cause(FCR31_CAUSE_UNIMP);
      if (cpu->_fp_exception_enabled(FCR31_CAUSE_UNIMP)) cpu->_raise_exception(EXC_FPE, old_pc);
    }
  }
}


// ── UltraHLE patch handlers (full surface 1..61) ──
static const char* _uh_skip(UltraHleOs*, CPUCore*, u64*) { return nullptr; }
static const char* _uh_osskip(UltraHleOs*, CPUCore*, u64*) { return nullptr; }
static const char* _uh_osskip0(UltraHleOs*, CPUCore*, u64* g) { g[_UH_V0]=0; return nullptr; }
static const char* _uh_dabort(UltraHleOs*, CPUCore*, u64*) { return nullptr; }
static const char* _uh_ddivu(UltraHleOs*, CPUCore*, u64* g) {
  u64 a=(u64m(g[_UH_A0])<<32)|u64m(g[_UH_A1]&MASK_32);
  u64 b=(u64m(g[_UH_A2])<<32)|u64m(g[_UH_A3]&MASK_32);
  u64 r=b?a/b:0; g[_UH_V0]=u64m(r>>32); g[_UH_V1]=u64m(r&MASK_32); return nullptr;
}
static const char* _uh_dmultu(UltraHleOs*, CPUCore*, u64* g) {
  // Match Python: 64-bit product truncated, then split into V0:V1
  u64 a=(u64m(g[_UH_A0])<<32)|u64m(g[_UH_A1]&MASK_32);
  u64 b=(u64m(g[_UH_A2])<<32)|u64m(g[_UH_A3]&MASK_32);
  u64 r=a*b; g[_UH_V0]=u64m(r>>32); g[_UH_V1]=u64m(r&MASK_32); return nullptr;
}
static const char* _uh_drem(UltraHleOs*, CPUCore*, u64* g) {
  u64 a=(u64m(g[_UH_A0])<<32)|u64m(g[_UH_A1]&MASK_32);
  u64 b=(u64m(g[_UH_A2])<<32)|u64m(g[_UH_A3]&MASK_32);
  u64 r=b?a%b:0; g[_UH_V0]=u64m(r>>32); g[_UH_V1]=u64m(r&MASK_32); return nullptr;
}
static const char* _uh_ddiv(UltraHleOs*, CPUCore*, u64* g) {
  s64 a=sign64((u64m(g[_UH_A0])<<32)|u64m(g[_UH_A1]&MASK_32));
  s64 b=sign64((u64m(g[_UH_A2])<<32)|u64m(g[_UH_A3]&MASK_32));
  s64 r=b?a/b:0; g[_UH_V0]=u64m((u64)r>>32); g[_UH_V1]=u64m((u64)r&MASK_32); return nullptr;
}
static const char* _uh_osCreateThread(UltraHleOs* hle, CPUCore*, u64* g) {
  hle->create_thread(u32m(g[_UH_A0]), u32m(g[_UH_A1]), u32m(g[_UH_A2]), u32m(g[_UH_A3]),
                     hle->_sp_arg(g,0), hle->_sp_arg(g,1));
  return nullptr;
}
static const char* _uh_osStartThread(UltraHleOs* hle, CPUCore*, u64* g) {
  hle->start_thread(u32m(g[_UH_A0])); return "switched";
}
static const char* _uh_osStopCurrentThread(UltraHleOs* hle, CPUCore*, u64*) {
  hle->stop_current_thread(); return "switched";
}
static const char* _uh_osSetThreadPri(UltraHleOs* hle, CPUCore* cpu, u64* g) {
  u32 m_thr=u32m(g[_UH_A0]); u32 pri=u32m(g[_UH_A1]);
  int tid = (m_thr==0) ? hle->current_thread : (int)hle->_rd32(m_thr);
  if (hle->threads.count(tid)) {
    hle->threads[tid].pri = (int)pri * 10;
    u32 ra=u32m(g[_UH_RA]); cpu->pc=ra; cpu->next_pc=u32m(ra+4);
    hle->schedule(tid); return "switched";
  }
  return nullptr;
}
static const char* _uh_osPiStartDma(UltraHleOs* hle, CPUCore*, u64* g) {
  u32 direction=u32m(g[_UH_A2]), dev=u32m(g[_UH_A3]);
  u32 vaddr=hle->_sp_arg(g,0), nbytes=hle->_sp_arg(g,1), mq=hle->_sp_arg(g,2);
  auto* bus=hle->_bus(); u32 cart=dev;
  if (cart < 0x10000000u) cart = u32m(0x10000000u + (cart & 0x0FFFFFFFu));
  bus->regs[PI_DRAM_ADDR] = hle->virt_to_phys(vaddr) & 0x00FFFFFF;
  bus->regs[PI_CART_ADDR] = cart;
  if (direction==0) { bus->regs[PI_WR_LEN]=std::max(0,(int)nbytes-1); bus->regs[PI_RD_LEN]=0; }
  else { bus->regs[PI_RD_LEN]=std::max(0,(int)nbytes-1); bus->regs[PI_WR_LEN]=0; }
  hle->core->trigger_pi_dma();
  if (mq) hle->os_send_mesg(mq, 0, -1);
  hle->os_event(OS_EVENT_PI); g[_UH_V0]=0; return nullptr;
}
static const char* _uh_osEPiStartDma(UltraHleOs* hle, CPUCore*, u64* g) {
  u32 mb=u32m(g[_UH_A1]); if (!mb) { g[_UH_V0]=u64m((u64)-1); return nullptr; }
  u32 mq=hle->_rd32(mb+4), vaddr=hle->_rd32(mb+8), dev=hle->_rd32(mb+12), nbytes=hle->_rd32(mb+16);
  u32 direction=hle->_rd32(mb)&0xFF; auto* bus=hle->_bus(); u32 cart=dev;
  if (cart < 0x10000000u) cart = u32m(0x10000000u + (cart & 0x0FFFFFFFu));
  bus->regs[PI_DRAM_ADDR]=hle->virt_to_phys(vaddr)&0x00FFFFFF; bus->regs[PI_CART_ADDR]=cart;
  if (direction==0) { bus->regs[PI_WR_LEN]=std::max(0,(int)nbytes-1); bus->regs[PI_RD_LEN]=0; }
  else { bus->regs[PI_RD_LEN]=std::max(0,(int)nbytes-1); bus->regs[PI_WR_LEN]=0; }
  hle->core->trigger_pi_dma();
  if (mq) hle->os_send_mesg(mq, mb, -1);
  hle->os_event(OS_EVENT_PI); g[_UH_V0]=0; return nullptr;
}
static const char* _uh_osCreateMesgQueue(UltraHleOs* hle, CPUCore*, u64* g) {
  hle->os_create_mesg_queue(u32m(g[_UH_A0]), u32m(g[_UH_A1]), (int)g[_UH_A2]); return nullptr;
}
static const char* _uh_osRecvMesg(UltraHleOs* hle, CPUCore* cpu, u64* g) {
  hle->block_pc=0;
  int rc=hle->os_recv_mesg(u32m(g[_UH_A0]), u32m(g[_UH_A1]), (int)g[_UH_A2]);
  if (hle->block_pc) {
    u32 patch_pc=u32m(cpu->_patch_pc ? cpu->_patch_pc : cpu->pc);
    cpu->pc=patch_pc; cpu->next_pc=u32m(patch_pc+4);
    if (hle->threads.count(hle->current_thread)) {
      auto& cur=hle->threads[hle->current_thread];
      cur.ready=0; cur.pc=patch_pc; cur.next_pc=u32m(patch_pc+4);
      std::memcpy(cur.gpr.data(), cpu->gpr, sizeof(cpu->gpr)); cur.hi=cpu->hi; cur.lo=cpu->lo;
    }
    g[_UH_V0]=0; cpu->gpr[_UH_V0]=0;
    cpu->cp0[CP0_COUNT]=u32m(cpu->cp0[CP0_COUNT]+64);
    if (hle->schedule(0)) return "switched";
    return "block";
  }
  g[_UH_V0]=sx32_to_64((u32)rc); return nullptr;
}
static const char* _uh_osSendMesg(UltraHleOs* hle, CPUCore*, u64* g) {
  g[_UH_V0]=sx32_to_64((u32)hle->os_send_mesg(u32m(g[_UH_A0]), u32m(g[_UH_A1]), (int)g[_UH_A2]));
  return nullptr;
}
static const char* _uh_osSetEventMessage(UltraHleOs* hle, CPUCore*, u64* g) {
  hle->os_set_event_mesg((int)g[_UH_A0], u32m(g[_UH_A1]), u32m(g[_UH_A2])); return nullptr;
}
static const char* _uh_osViSetEvent(UltraHleOs* hle, CPUCore*, u64* g) {
  hle->os_set_event_mesg(OS_EVENT_RETRACE, u32m(g[_UH_A0]), u32m(g[_UH_A1])); return nullptr;
}
static const char* _uh_osGetTime(UltraHleOs* hle, CPUCore* cpu, u64* g) {
  hle->tick_time(cpu->cp0[CP0_COUNT] & MASK_32);
  g[_UH_V0]=u64m(hle->time_hi); g[_UH_V1]=u64m(hle->time_lo); return nullptr;
}
static const char* _uh_osGetCount(UltraHleOs*, CPUCore* cpu, u64* g) {
  g[_UH_V0]=sx32_to_64(cpu->cp0[CP0_COUNT]); return nullptr;
}
static const char* _uh_osViGetCurrentFrameBuffer(UltraHleOs* hle, CPUCore*, u64* g) {
  u32 vo = hle->_bus()->regs.count(VI_ORIGIN) ? hle->_bus()->regs[VI_ORIGIN] : 0;
  g[_UH_V0]=u64m(hle->fb_current ? hle->fb_current : vo); return nullptr;
}
static const char* _uh_osViSwapBuffer(UltraHleOs* hle, CPUCore*, u64* g) {
  // Project64-Legacy: VI swap hands a framebuffer to the graphics plugin for present.
  hle->fb_next=u32m(g[_UH_A0]); hle->fb_current=hle->fb_next;
  u32 phys = hle->virt_to_phys(hle->fb_current) & 0x00FFFFFF;
  hle->_bus()->regs[VI_ORIGIN] = phys;
  int w = (int)((hle->_bus()->regs.count(VI_WIDTH) ? hle->_bus()->regs[VI_WIDTH] : 320) & 0xFFF);
  if (w < 16 || w > 640) w = 320;
  // Wipe swap buffer so we never show RDRAM snow; mark first lit frame ready.
  u16 col = hle->core->rdp.fill_color ? hle->core->rdp.fill_color : (u16)0x0001;
  hle->core->rdp.present_buffer(phys, w, 240, col);
  hle->core->fb_lit = true;
  hle->core->boot_turbo = false;
  hle->core->_vi_origin_set = true;
  hle->core->render_vi();
  return nullptr;
}
static const char* _uh_osVirtualToPhysical(UltraHleOs* hle, CPUCore*, u64* g) {
  g[_UH_V0]=u64m(hle->virt_to_phys(u32m(g[_UH_A0]))); return nullptr;
}
static const char* _uh_osPhysicalToVirtual(UltraHleOs* hle, CPUCore*, u64* g) {
  g[_UH_V0]=u64m(hle->phys_to_virt(u32m(g[_UH_A0]))); return nullptr;
}
static const char* _uh_osSpTaskStartGo(UltraHleOs* hle, CPUCore*, u64* g) {
  u32 task=u32m(g[_UH_A0]);
  if (!hle->sptaskload) {
    if (task) { put_be32(hle->core->rsp_dmem, 0xFC0, hle->_rd32(task)); hle->core->last_os_task=task; }
    hle->core->process_rsp();
  } else if (task) hle->core->last_os_task=task;
  hle->sptaskload=false; hle->os_event(OS_EVENT_SP); hle->os_event(OS_EVENT_DP); return nullptr;
}
static const char* _uh_osSpTaskLoad(UltraHleOs* hle, CPUCore*, u64* g) {
  u32 task=u32m(g[_UH_A0]);
  if (task) { put_be32(hle->core->rsp_dmem, 0xFC0, hle->_rd32(task)); hle->core->last_os_task=task; }
  hle->core->process_rsp(); hle->sptaskload=true; return nullptr;
}
static const char* _uh_osSpTaskYield(UltraHleOs*, CPUCore*, u64* g) { g[_UH_V0]=0; return nullptr; }
static const char* _uh_osSpTaskYielded(UltraHleOs*, CPUCore*, u64* g) { g[_UH_V0]=0; return nullptr; }
static const char* _uh_osMapTLB(UltraHleOs* hle, CPUCore* cpu, u64* g) {
  u32 idx=u32m(g[_UH_A0])&0x1F, pmask=u32m(g[_UH_A1]), vaddr=u32m(g[_UH_A2]), even_p=u32m(g[_UH_A3]);
  u32 odd_p=hle->_sp_arg(g,0), asid=hle->_sp_arg(g,1)&0xFF;
  cpu->cp0[CP0_INDEX]=idx; cpu->cp0[CP0_PAGEMASK]=pmask;
  cpu->cp0[CP0_ENTRYHI]=(vaddr&0xFFFFE000u)|asid;
  cpu->cp0[CP0_ENTRYLO0]=((even_p>>12)<<6)|0x06;
  cpu->cp0[CP0_ENTRYLO1]=((odd_p>>12)<<6)|0x06;
  cpu->_write_tlb_entry((int)idx); return nullptr;
}
static const char* _uh_osContInit(UltraHleOs* hle, CPUCore*, u64* g) {
  u32 status=u32m(g[_UH_A1]); if (status) hle->_wr32(status, 0x05000200); g[_UH_V0]=0; return nullptr;
}
static const char* _uh_osContStartReadData(UltraHleOs* hle, CPUCore*, u64* g) {
  hle->os_event(OS_EVENT_SI); g[_UH_V0]=0; return nullptr;
}
static const char* _uh_osContGetReadData(UltraHleOs* hle, CPUCore*, u64* g) {
  u32 pad=u32m(g[_UH_A0]); if (!pad) return nullptr;
  auto* bus=hle->_bus();
  for (int i=0;i<4;i++) {
    u32 base=pad+i*6; u16 btn=(i==0)?(u16)(hle->cont_pad&0xFFFF):0;
    bus->write_u16(base, btn); bus->write_u8(base+2,0); bus->write_u8(base+3,0); bus->write_u8(base+4,0);
  }
  return nullptr;
}
static const char* _uh_osContStartQuery(UltraHleOs* hle, CPUCore*, u64* g) {
  hle->os_event(OS_EVENT_SI); g[_UH_V0]=0; return nullptr;
}
static const char* _uh_osContGetQuery(UltraHleOs* hle, CPUCore*, u64* g) {
  u32 status=u32m(g[_UH_A0]); if (status) hle->_wr32(status, 0x05000200); return nullptr;
}
static const char* _uh_osAiSetNextBuffer(UltraHleOs* hle, CPUCore*, u64* g) {
  hle->ai_buf=u32m(g[_UH_A0]); hle->ai_len=u32m(g[_UH_A1]);
  hle->_bus()->regs[AI_DRAM_ADDR]=hle->ai_buf&0x00FFFFFF;
  hle->_bus()->regs[AI_LEN]=hle->ai_len;
  hle->core->process_audio(); hle->os_event(OS_EVENT_AI); g[_UH_V0]=0; return nullptr;
}
static const char* _uh_osAiGetLength(UltraHleOs* hle, CPUCore*, u64* g) {
  g[_UH_V0]=u64m(hle->_bus()->regs.count(AI_LEN)?hle->_bus()->regs[AI_LEN]:0); return nullptr;
}
static const char* _uh_osAiSetFrequency(UltraHleOs* hle, CPUCore*, u64* g) {
  hle->ai_freq=u32m(g[_UH_A0])?u32m(g[_UH_A0]):32000; g[_UH_V0]=u64m(hle->ai_freq); return nullptr;
}
static const char* _uh_osSetTimer(UltraHleOs* hle, CPUCore* cpu, u64* g) {
  u32 timer=u32m(g[_UH_A0]), countdown=u32m(g[_UH_A3]);
  u32 interval=u32m(hle->_sp_arg(g,1)), mq=hle->_sp_arg(g,2), msg=hle->_sp_arg(g,3);
  u32 now=u32m(cpu->cp0[CP0_COUNT]); u32 delay=countdown?countdown:1;
  UhTimer t; t.active=true; t.expire=u32m(now+delay); t.interval=interval; t.mq=mq; t.msg=msg;
  hle->timers[timer]=t; g[_UH_V0]=0; return nullptr;
}
static const char* _uh_osStopTimer(UltraHleOs* hle, CPUCore*, u64* g) {
  u32 timer=u32m(g[_UH_A0]); if (hle->timers.count(timer)) hle->timers[timer].active=false; return nullptr;
}
static const char* _uh_sinf(UltraHleOs*, CPUCore* cpu, u64*) {
  float x=bits_to_f32((u32)(cpu->fpr[12]&MASK_32));
  cpu->fpr[0]=u64m((cpu->fpr[0]&0xFFFFFFFF00000000ull)|f32_to_bits(std::sin(x))); return nullptr;
}
static const char* _uh_cosf(UltraHleOs*, CPUCore* cpu, u64*) {
  float x=bits_to_f32((u32)(cpu->fpr[12]&MASK_32));
  cpu->fpr[0]=u64m((cpu->fpr[0]&0xFFFFFFFF00000000ull)|f32_to_bits(std::cos(x))); return nullptr;
}
static const char* _uh_osInvalICache(UltraHleOs*, CPUCore*, u64*) { return nullptr; }
static const char* _uh_readdma(UltraHleOs* hle, CPUCore*, u64* g) {
  auto* bus=hle->_bus(); u32 cart=u32m(g[_UH_A0]);
  if (cart < 0x10000000u) cart=u32m(0x10000000u+(cart&0x0FFFFFFFu));
  bus->regs[PI_CART_ADDR]=cart;
  bus->regs[PI_DRAM_ADDR]=hle->virt_to_phys(u32m(g[_UH_A1]))&0x00FFFFFF;
  bus->regs[PI_WR_LEN]=std::max(0,(int)u32m(g[_UH_A2])-1); bus->regs[PI_RD_LEN]=0;
  hle->core->trigger_pi_dma(); hle->os_event(OS_EVENT_PI); return nullptr;
}
static const char* _uh_readdma2(UltraHleOs* hle, CPUCore*, u64* g) {
  auto* bus=hle->_bus();
  bus->regs[PI_CART_ADDR]=u32m(g[_UH_A0]);
  bus->regs[PI_DRAM_ADDR]=hle->virt_to_phys(u32m(g[_UH_A1]))&0x00FFFFFF;
  bus->regs[PI_WR_LEN]=std::max(0,(int)u32m(g[_UH_A2])-1);
  hle->core->trigger_pi_dma(); return nullptr;
}
static const char* _uh_zeldacont(UltraHleOs* hle, CPUCore*, u64*) { hle->os_event(OS_EVENT_SI); return nullptr; }
static const char* _uh_zeldagrabscreen(UltraHleOs*, CPUCore*, u64*) { return nullptr; }
static const char* _uh_banjojalr(UltraHleOs*, CPUCore*, u64*) { return nullptr; }
static const char* _uh_long2double(UltraHleOs*, CPUCore* cpu, u64* g) {
  s64 a=sign64((u64m(g[_UH_A0])<<32)|u64m(g[_UH_A1]&MASK_32));
  cpu->fpr[0]=f64_to_bits((double)a); return nullptr;
}
static const char* _uh_long2single(UltraHleOs*, CPUCore* cpu, u64* g) {
  s64 a=sign64((u64m(g[_UH_A0])<<32)|u64m(g[_UH_A1]&MASK_32));
  cpu->fpr[0]=u64m((cpu->fpr[0]&0xFFFFFFFF00000000ull)|f32_to_bits((float)a)); return nullptr;
}
static const char* _uh_double2long(UltraHleOs*, CPUCore* cpu, u64* g) {
  double d=bits_to_f64(cpu->fpr[12]); s64 a=std::isfinite(d)?(s64)d:0;
  g[_UH_V1]=u64m((u64)a&MASK_32); g[_UH_V0]=u64m(((u64)a>>32)&MASK_32); return nullptr;
}
static const char* _uh_single2long(UltraHleOs*, CPUCore* cpu, u64* g) {
  float f=bits_to_f32((u32)(cpu->fpr[12]&MASK_32)); s64 a=std::isfinite(f)?(s64)f:0;
  g[_UH_V1]=u64m((u64)a&MASK_32); g[_UH_V0]=u64m(((u64)a>>32)&MASK_32); return nullptr;
}
static const char* _uh_golden1(UltraHleOs*, CPUCore*, u64* g) {
  static std::mt19937 rng{0xCA711Eu};
  u32 x=(u32)rng(); x=u32m((x<<8)|(x>>24)); g[_UH_V0]=sx32_to_64(x); return nullptr;
}
static const char* _uh_createvimanager(UltraHleOs*, CPUCore*, u64* g) { g[_UH_V0]=0; return nullptr; }
static const char* _uh_memcpy(UltraHleOs* hle, CPUCore*, u64* g) {
  u32 dst=u32m(g[_UH_A0]), src=u32m(g[_UH_A1]), count=u32m(g[_UH_A2])>>2;
  auto* bus=hle->_bus();
  for (u32 i=0;i<count;i++) { bus->write_u32(dst, bus->read_u32(src)); dst=u32m(dst+4); src=u32m(src+4); }
  return nullptr;
}
static const char* _uh_osEepromProbe(UltraHleOs* hle, CPUCore*, u64* g) {
  int typ=hle->core->save_mgr.eeprom_probe(); hle->os_event(OS_EVENT_SI); g[_UH_V0]=typ; return nullptr;
}
static const char* _uh_osEepromRead(UltraHleOs* hle, CPUCore*, u64* g) {
  int block=u32m(g[_UH_A1])&0xFF; u32 buf=u32m(g[_UH_A2]); u8 tmp[8];
  int rc=hle->core->save_mgr.eeprom_read_block(block, tmp);
  if (rc==0 && buf) { auto* bus=hle->_bus(); for(int i=0;i<8;i++) bus->write_u8(u32m(buf+i), tmp[i]); }
  hle->os_event(OS_EVENT_SI); g[_UH_V0]=rc==0?0:u64m((u64)-1); return nullptr;
}
static const char* _uh_osEepromWrite(UltraHleOs* hle, CPUCore*, u64* g) {
  int block=u32m(g[_UH_A1])&0xFF; u32 buf=u32m(g[_UH_A2]); u8 tmp[8]{};
  if (buf) { auto* bus=hle->_bus(); for(int i=0;i<8;i++) tmp[i]=bus->read_u8(u32m(buf+i)); }
  int rc=hle->core->save_mgr.eeprom_write_block(block, tmp);
  hle->os_event(OS_EVENT_SI); g[_UH_V0]=rc==0?0:u64m((u64)-1); return nullptr;
}
static const char* _uh_osEepromLongRead(UltraHleOs* hle, CPUCore*, u64* g) {
  int block=u32m(g[_UH_A1])&0xFF; u32 buf=u32m(g[_UH_A2]), nbytes=u32m(g[_UH_A3]);
  int rc=-1;
  if (buf) {
    int nblocks=(int)((nbytes+7)/8); rc=0;
    for (int i=0;i<nblocks;i++) {
      u8 tmp[8]; if (hle->core->save_mgr.eeprom_read_block(block+i,tmp)!=0) { rc=-1; break; }
      for (int b=0;b<8;b++) hle->_bus()->write_u8(u32m(buf+i*8+b), tmp[b]);
    }
  }
  hle->os_event(OS_EVENT_SI); g[_UH_V0]=rc==0?0:u64m((u64)-1); return nullptr;
}
static const char* _uh_osEepromLongWrite(UltraHleOs* hle, CPUCore*, u64* g) {
  int block=u32m(g[_UH_A1])&0xFF; u32 buf=u32m(g[_UH_A2]), nbytes=u32m(g[_UH_A3]);
  int rc=-1;
  if (buf) {
    int nblocks=(int)((nbytes+7)/8); rc=0;
    for (int i=0;i<nblocks;i++) {
      u8 tmp[8]; for (int b=0;b<8;b++) tmp[b]=hle->_bus()->read_u8(u32m(buf+i*8+b));
      if (hle->core->save_mgr.eeprom_write_block(block+i,tmp)!=0) { rc=-1; break; }
    }
  }
  hle->os_event(OS_EVENT_SI); g[_UH_V0]=rc==0?0:u64m((u64)-1); return nullptr;
}

static void _h_PATCH(CPUCore* cpu, const N64Opcode& o, u32 old_pc, u64* g) {
  int patch = o.imm & 0xFFFF;
  if (patch == 49) {
    auto it = ULTRAHLE_PATCH_TABLE.find(49);
    if (it != ULTRAHLE_PATCH_TABLE.end()) it->second(&cpu->core->ultrahle, cpu, g);
    return;
  }
  if (patch <= 2) {
    // UltraHLE: EnqueueAndYield / yield sites schedule another ready thread instead of spinning.
    if (patch == 1 && cpu->core->uh_sym.yield_addrs.count(u32m(old_pc))) {
      auto& hle = cpu->core->ultrahle;
      auto it = hle.threads.find(hle.current_thread);
      if (it != hle.threads.end()) {
        u32 ra = u32m(g[_UH_RA]);
        it->second.pc = ra; it->second.next_pc = u32m(ra + 4);
        std::memcpy(it->second.gpr.data(), cpu->gpr, sizeof(cpu->gpr));
        it->second.hi = cpu->hi; it->second.lo = cpu->lo;
      }
      if (hle.schedule(0)) return;
    }
    u32 ra = u32m(g[_UH_RA]); cpu->pc = ra; cpu->next_pc = u32m(ra + 4); return;
  }
  cpu->_patch_pc = u32m(old_pc);
  const char* blocked = nullptr;
  auto it = ULTRAHLE_PATCH_TABLE.find(patch);
  if (it != ULTRAHLE_PATCH_TABLE.end()) blocked = it->second(&cpu->core->ultrahle, cpu, g);
  if (blocked && std::string(blocked) == "switched") return;
  if (blocked && std::string(blocked) == "block") {
    cpu->pc = u32m(old_pc); cpu->next_pc = u32m(old_pc + 4); return;
  }
  u32 ra = u32m(g[_UH_RA]); cpu->pc = ra; cpu->next_pc = u32m(ra + 4);
  cpu->core->ultrahle.maybe_preempt();
}

static void init_ultrahle_tables() {
  ULTRAHLE_PATCH_NAMES = {
    {1,"skip"},{2,"osskip"},{3,"__ull_div"},{4,"__ll_mul"},{5,"__ull_rem"},{6,"__ll_div"},{7,"__ll_rem"},
    {8,"osskip0"},{9,"dabort"},{10,"osCreateThread"},{11,"osStartThread"},{12,"osPiStartDma"},
    {13,"osCreateMesgQueue"},{14,"osRecvMesg"},{15,"osSendMesg"},{16,"osSetEventMesg"},{17,"osViSetEvent"},
    {18,"osSetThreadPri"},{19,"osGetTime"},{20,"osViGetCurrentFramebuffer"},{21,"osVirtualToPhysical"},
    {22,"osSpTaskStartGo"},{23,"osViSwapBuffer"},{24,"osMapTLB"},{25,"osContInit"},{26,"osContStartReadData"},
    {27,"osContGetReadData"},{28,"osStopCurrentThread"},{29,"osPhysicalToVirtual"},{30,"osAiSetNextBuffer"},
    {31,"osAiGetLength"},{32,"osAiSetFrequency"},{33,"osSetTimer"},{34,"osStopTimer"},{35,"sinf"},{36,"cosf"},
    {37,"osEPiStartDma"},{38,"osInvalICache"},{39,"osSpTaskLoad"},{40,"readdma"},{41,"readdma2"},
    {42,"osContStartQuery"},{43,"osContGetQuery"},{44,"zeldacont"},{45,"zeldagrabscreen"},
    {46,"osSpTaskYield"},{47,"osSpTaskYielded"},{48,"osGetCount"},{49,"banjojalr"},
    {50,"long2double"},{51,"long2single"},{52,"double2long"},{53,"single2long"},{54,"golden1"},
    {55,"createViManager"},{56,"memcpy"},{57,"osEepromProbe"},{58,"osEepromRead"},{59,"osEepromWrite"},
    {60,"osEepromLongRead"},{61,"osEepromLongWrite"},
  };
  ULTRAHLE_PATCH_TABLE = {
    {1,_uh_skip},{2,_uh_osskip},{3,_uh_ddivu},{4,_uh_dmultu},{5,_uh_drem},{6,_uh_ddiv},{7,_uh_drem},
    {8,_uh_osskip0},{9,_uh_dabort},{10,_uh_osCreateThread},{11,_uh_osStartThread},{12,_uh_osPiStartDma},
    {13,_uh_osCreateMesgQueue},{14,_uh_osRecvMesg},{15,_uh_osSendMesg},{16,_uh_osSetEventMessage},{17,_uh_osViSetEvent},
    {18,_uh_osSetThreadPri},{19,_uh_osGetTime},{20,_uh_osViGetCurrentFrameBuffer},{21,_uh_osVirtualToPhysical},
    {22,_uh_osSpTaskStartGo},{23,_uh_osViSwapBuffer},{24,_uh_osMapTLB},{25,_uh_osContInit},{26,_uh_osContStartReadData},
    {27,_uh_osContGetReadData},{28,_uh_osStopCurrentThread},{29,_uh_osPhysicalToVirtual},{30,_uh_osAiSetNextBuffer},
    {31,_uh_osAiGetLength},{32,_uh_osAiSetFrequency},{33,_uh_osSetTimer},{34,_uh_osStopTimer},{35,_uh_sinf},{36,_uh_cosf},
    {37,_uh_osEPiStartDma},{38,_uh_osInvalICache},{39,_uh_osSpTaskLoad},{40,_uh_readdma},{41,_uh_readdma2},
    {42,_uh_osContStartQuery},{43,_uh_osContGetQuery},{44,_uh_zeldacont},{45,_uh_zeldagrabscreen},
    {46,_uh_osSpTaskYield},{47,_uh_osSpTaskYielded},{48,_uh_osGetCount},{49,_uh_banjojalr},
    {50,_uh_long2double},{51,_uh_long2single},{52,_uh_double2long},{53,_uh_single2long},{54,_uh_golden1},
    {55,_uh_createvimanager},{56,_uh_memcpy},{57,_uh_osEepromProbe},{58,_uh_osEepromRead},{59,_uh_osEepromWrite},
    {60,_uh_osEepromLongRead},{61,_uh_osEepromLongWrite},
  };
}

static void init_dispatch() {
  std::memset(_DISPATCH, 0, sizeof(_DISPATCH));
  auto setp=[&](int op, HandlerFn fn){ _DISPATCH[_ID_PRIMARY|op]=fn; };
  auto sets=[&](int f, HandlerFn fn){ _DISPATCH[_ID_SPECIAL|f]=fn; };
  auto setr=[&](int rt, HandlerFn fn){ _DISPATCH[_ID_REGIMM|rt]=fn; };
  setp(0x02,_h_J); setp(0x03,_h_JAL); setp(0x04,_h_BEQ); setp(0x05,_h_BNE); setp(0x06,_h_BLEZ); setp(0x07,_h_BGTZ);
  setp(0x08,_h_ADDI); setp(0x09,_h_ADDIU); setp(0x0A,_h_SLTI); setp(0x0B,_h_SLTIU); setp(0x0C,_h_ANDI); setp(0x0D,_h_ORI); setp(0x0E,_h_XORI); setp(0x0F,_h_LUI);
  setp(0x14,_h_BEQL); setp(0x15,_h_BNEL); setp(0x16,_h_BLEZL); setp(0x17,_h_BGTZL);
  setp(0x18,_h_DADDI); setp(0x19,_h_DADDIU); setp(0x1A,_h_LDL); setp(0x1B,_h_LDR);
  setp(0x1C,_h_PATCH); setp(0x1D,_h_GROUP);
  setp(0x20,_h_LB); setp(0x21,_h_LH); setp(0x22,_h_LWL); setp(0x23,_h_LW); setp(0x24,_h_LBU); setp(0x25,_h_LHU); setp(0x26,_h_LWR); setp(0x27,_h_LWU);
  setp(0x28,_h_SB); setp(0x29,_h_SH); setp(0x2A,_h_SWL); setp(0x2B,_h_SW); setp(0x2C,_h_SDL); setp(0x2D,_h_SDR); setp(0x2E,_h_SWR); setp(0x2F,_h_CACHE);
  setp(0x30,_h_LL); setp(0x31,_h_LWC1); setp(0x34,_h_LLD); setp(0x35,_h_LDC1); setp(0x37,_h_LD);
  setp(0x38,_h_SC); setp(0x39,_h_SWC1); setp(0x3C,_h_SCD); setp(0x3D,_h_SDC1); setp(0x3F,_h_SD);
  sets(0x00,_h_SLL); sets(0x02,_h_SRL); sets(0x03,_h_SRA); sets(0x04,_h_SLLV); sets(0x06,_h_SRLV); sets(0x07,_h_SRAV);
  sets(0x08,_h_JR); sets(0x09,_h_JALR); sets(0x0A,_h_MOVZ); sets(0x0B,_h_MOVN); sets(0x0C,_h_SYSCALL); sets(0x0D,_h_BREAK); sets(0x0F,_h_SYNC);
  sets(0x10,_h_MFHI); sets(0x11,_h_MTHI); sets(0x12,_h_MFLO); sets(0x13,_h_MTLO);
  sets(0x14,_h_DSLLV); sets(0x16,_h_DSRLV); sets(0x17,_h_DSRAV);
  sets(0x18,_h_MULT); sets(0x19,_h_MULTU); sets(0x1A,_h_DIV); sets(0x1B,_h_DIVU); sets(0x1C,_h_DMULT); sets(0x1D,_h_DMULTU); sets(0x1E,_h_DDIV); sets(0x1F,_h_DDIVU);
  sets(0x20,_h_ADD); sets(0x21,_h_ADDU); sets(0x22,_h_SUB); sets(0x23,_h_SUBU); sets(0x24,_h_AND); sets(0x25,_h_OR); sets(0x26,_h_XOR); sets(0x27,_h_NOR);
  sets(0x2A,_h_SLT); sets(0x2B,_h_SLTU); sets(0x2C,_h_DADD); sets(0x2D,_h_DADDU); sets(0x2E,_h_DSUB); sets(0x2F,_h_DSUBU);
  sets(0x30,_h_TGE); sets(0x31,_h_TGEU); sets(0x32,_h_TLT); sets(0x33,_h_TLTU); sets(0x34,_h_TEQ); sets(0x36,_h_TNE);
  sets(0x38,_h_DSLL); sets(0x3A,_h_DSRL); sets(0x3B,_h_DSRA); sets(0x3C,_h_DSLL32); sets(0x3E,_h_DSRL32); sets(0x3F,_h_DSRA32);
  for (int f : {0x01,0x05,0x0E,0x15,0x28,0x29,0x35,0x37,0x39,0x3D}) sets(f, _h_RI);
  setr(0x00,_h_BLTZ); setr(0x01,_h_BGEZ); setr(0x02,_h_BLTZL); setr(0x03,_h_BGEZL);
  setr(0x08,_h_TGEI); setr(0x09,_h_TGEIU); setr(0x0A,_h_TLTI); setr(0x0B,_h_TLTIU); setr(0x0C,_h_TEQI); setr(0x0E,_h_TNEI);
  setr(0x10,_h_BLTZAL); setr(0x11,_h_BGEZAL); setr(0x12,_h_BLTZALL); setr(0x13,_h_BGEZALL);
  for (int rt : {0x04,0x05,0x06,0x07,0x0D,0x0F}) setr(rt, _h_RI);
  for (int rt = 0x14; rt < 0x20; ++rt) setr(rt, _h_RI);
  // Fill every COP0/COP1 sub-slot so decode never falls through to a bogus CpU path.
  for (int rs = 0; rs < 32; ++rs) _DISPATCH[_ID_COP0_RS|rs] = _h_RI;
  for (int f = 0; f < 64; ++f) _DISPATCH[_ID_COP0_CO|f] = _h_RI;
  for (int rs = 0; rs < 32; ++rs) _DISPATCH[_ID_COP1_RS|rs] = _h_RI;
  _DISPATCH[_ID_COP0_RS|0x00]=_h_MFC0; _DISPATCH[_ID_COP0_RS|0x01]=_h_DMFC0; _DISPATCH[_ID_COP0_RS|0x02]=_h_CFC0;
  _DISPATCH[_ID_COP0_RS|0x04]=_h_MTC0; _DISPATCH[_ID_COP0_RS|0x05]=_h_DMTC0; _DISPATCH[_ID_COP0_RS|0x06]=_h_CTC0; _DISPATCH[_ID_COP0_RS|0x08]=_h_BC0;
  _DISPATCH[_ID_COP0_CO|0x01]=_h_TLBR; _DISPATCH[_ID_COP0_CO|0x02]=_h_TLBWI; _DISPATCH[_ID_COP0_CO|0x06]=_h_TLBWR;
  _DISPATCH[_ID_COP0_CO|0x08]=_h_TLBP; _DISPATCH[_ID_COP0_CO|0x18]=_h_ERET; _DISPATCH[_ID_COP0_CO|0x20]=_h_WAIT;
  _DISPATCH[_ID_COP1_RS|0x00]=_h_MFC1; _DISPATCH[_ID_COP1_RS|0x01]=_h_DMFC1; _DISPATCH[_ID_COP1_RS|0x02]=_h_CFC1;
  _DISPATCH[_ID_COP1_RS|0x03]=_h_DCFC1; // Legacy DCF
  _DISPATCH[_ID_COP1_RS|0x04]=_h_MTC1; _DISPATCH[_ID_COP1_RS|0x05]=_h_DMTC1; _DISPATCH[_ID_COP1_RS|0x06]=_h_CTC1;
  _DISPATCH[_ID_COP1_RS|0x07]=_h_DCTC1; // Legacy DCT
  _DISPATCH[_ID_COP1_RS|0x08]=_h_BC1;
  for (int rt = 0; rt < 32; ++rt) _DISPATCH[_ID_COP1_BC|rt]=_h_BC1;
  for (int fid : {_ID_FPU_S,_ID_FPU_D,_ID_FPU_W,_ID_FPU_L})
    for (int f = 0; f < 64; ++f) _DISPATCH[_ID_FPU|(fid<<6)|f]=_h_FPU;
  // COP2: Legacy R4300i_opcode_COP2 → CoP2[rs] (MF/DMF/CF/CT latch)
  setp(0x12, _h_COP2);
  setp(0x13, _h_CPU_UNUSABLE); // COP3 — CpU (Legacy r4300i_COP3)
  setp(0x1E,_h_RI); setp(0x1F,_h_RI);
  // LWC2/SWC2/LDC2/SDC2 → UnknownOpcode in Legacy; LWC3/SWC3 → RESERVED
  for (int op : {0x32,0x36,0x3A,0x3E}) setp(op, _h_RI); // Unknown → RI
  for (int op : {0x33,0x3B}) setp(op, _h_RI); // RESERVED
  // SPECIAL / REGIMM already filled; ensure every PRIMARY opcode slot is non-null
  // Note: 0x1C/0x1D stay PATCH/GROUP (UltraHLE); Legacy uses RESERVED there.
  for (int op = 0; op < 64; ++op) {
    if (!_DISPATCH[_ID_PRIMARY|op] && op != 0 && op != 1 && op != 0x10 && op != 0x11)
      _DISPATCH[_ID_PRIMARY|op] = _h_RI;
  }
}


// ── UltraHLE sym.c port (oscall / ospatch / CRC match → OP_PATCH) ──
u32 UltraHleSym::_rd32(u32 addr) { return core->bus.read_u32(u32m(addr)); }
void UltraHleSym::_wr32(u32 addr, u32 val) { core->bus.write_u32(u32m(addr), u32m(val)); }
bool UltraHleSym::_disabled(const std::string& name) const {
  std::string base = name; auto h = base.find('#'); if (h != std::string::npos) base = base.substr(0, h);
  while (!base.empty() && base.back()==' ') base.pop_back();
  for (size_t i=0;i<ULTRAHLE_DISABLE_PATCHES_N;i++) {
    std::string d = ULTRAHLE_DISABLE_PATCHES[i];
    if (base == d || base.rfind(d, 0) == 0) return true;
  }
  return false;
}
int UltraHleSym::add_symbol(u32 addr, const std::string& text, int patch) {
  addr &= ADDRMASK_UH; int p = patch;
  if (_disabled(text)) p = 0;
  for (size_t i=0;i<syms.size();i++) if (syms[i].addr == addr) { syms[i].text=text; syms[i].patch=p; return (int)i; }
  syms.push_back({addr, text, p, 0}); return (int)syms.size()-1;
}
std::string UltraHleSym::find_symbol_name(u32 addr) const {
  addr &= ADDRMASK_UH; const UhSymEntry* best=nullptr; u32 best_a=0;
  for (auto& s : syms) {
    if (s.addr == addr) return s.text;
    if (s.addr <= addr && s.addr >= best_a) { best_a = s.addr; best = &s; }
  }
  if (!best) return "?";
  u32 off = addr - best->addr;
  if (off > 99999) return "?";
  if (!off) return best->text;
  return "?" + std::to_string(off) + "+" + best->text;
}
std::pair<u32,u32> UltraHleSym::routine_crc2(u32 addr, u32 crc1_in) {
  u32 crc1 = crc1_in, crc2 = 0;
  u32 x1 = _rd32(addr);
  if (!(x1 & 0xFFFFFF)) return {0xFFFFFFFFu, 0xFFFFFFFFu};
  int inn = 16;
  for (int i=0;i<16;i++) { x1=_rd32(addr+i*4); if (x1==0x03E00008u) { inn=std::min(i+2,16); break; } }
  for (int i=0;i<inn;i++) {
    x1 = _rd32(addr+i*4); u32 crc = (u32)i; int op = _uh_op_op(x1);
    if (op==2 || op==3) crc += (u32)op;
    else if (op==15) {
      u32 imm = _uh_op_imm(x1);
      if (imm >= 0xA400 && imm <= 0xAFFF) crc2 = u32m(crc2 + x1); else crc += (u32)op;
    } else if (op==16 || op==17) { crc2 ^= x1; crc = 0; }
    else { if (crc1 & (1u<<i)) crc ^= (x1>>16); else crc ^= x1; }
    x1 = crc;
    if (inn < 4) { x1 ^= x1>>16; x1 ^= x1>>8; x1 = (x1 & 255u) << (i*8); }
    else if (inn < 8) { x1 ^= x1>>16; x1 ^= x1>>8; x1 ^= x1>>4; x1 = (x1 & 15u) << (i*4); }
    else { x1 ^= x1>>16; x1 ^= x1>>8; x1 ^= x1>>4; x1 ^= x1>>2; x1 = (x1 & 3u) << (i*2); }
    crc2 ^= x1;
  }
  return {crc1, u32m(crc2)};
}
void UltraHleSym::patch_names() {
  struct Pat { std::string npart; bool wild; int nlen; int pnum; };
  std::vector<Pat> patterns;
  for (size_t i=0;i<ULTRAHLE_OSPATCH_N;i++) {
    std::string entry = ULTRAHLE_OSPATCH[i];
    auto sp = entry.find(' '); if (sp==std::string::npos) continue;
    std::string namepart = entry.substr(0, sp);
    bool wild = !namepart.empty() && namepart.back()=='*';
    int nlen = (int)namepart.size() - (wild?1:0);
    int pnum = 0; auto hp = entry.find('#'); if (hp!=std::string::npos) pnum = std::atoi(entry.c_str()+hp+1);
    patterns.push_back({namepart.substr(0,(size_t)nlen), wild, nlen, pnum});
  }
  for (auto& s : syms) {
    if (s.text.find('%')!=std::string::npos) continue;
    if (s.text.find('#')!=std::string::npos && s.patch) continue;
    if (s.patch) continue;
    std::string base = s.text; auto h=base.find('#'); if(h!=std::string::npos) base=base.substr(0,h);
    while(!base.empty()&&base.back()==' ') base.pop_back();
    for (auto& p : patterns) {
      if ((int)base.size() < p.nlen) continue;
      std::string a=base.substr(0,(size_t)p.nlen), b=p.npart;
      for (char& c:a) c=(char)tolower((unsigned char)c);
      for (char& c:b) c=(char)tolower((unsigned char)c);
      if (a!=b) continue;
      if (!p.wild && (int)base.size()>p.nlen && base[(size_t)p.nlen] > ' ') continue;
      add_symbol(s.addr, base + " #" + std::to_string(p.pnum), p.pnum);
      break;
    }
  }
}
std::string UltraHleSym::find_os_calls(u32 base, u32 nbytes) {
  if (!nbytes) { last_report="empty range"; return last_report; }
  base = u32m(base | 0x80000000u); u32 end = u32m(base + nbytes);
  struct St { const OsCallSig* e; int flag; int symb; u32 found_at; int fclass; };
  std::vector<St> state; state.reserve(ULTRAHLE_OSCALL_N);
  for (size_t j=0;j<ULTRAHLE_OSCALL_N;j++) state.push_back({&ULTRAHLE_OSCALL[j],0,0,0,0});
  int total = (int)state.size();
  for (u32 i = end - 4; (int)(i - base) >= 0; i = u32m(i - 4)) {
    u32 x0 = _rd32(i); if (!(x0 & 0xFFFFFF)) continue;
    std::vector<int> hits;
    for (int j=0;j<total;j++) {
      auto& e = state[(size_t)j]; if (e.found_at) continue;
      u32 d0 = e.e->data[0]; if (d0 && ((x0 ^ d0) >> 16)) continue;
      bool ok=true;
      for (int k=1;k<8;k++) {
        u32 x=_rd32(i+k*4), y=e.e->data[k];
        if (y && ((x^y)>>16)) { ok=false; break; }
      }
      if (!ok) continue;
      if (!e.symb) e.symb = (int)i;
      e.flag = 1; hits.push_back(j);
    }
    if (hits.empty()) continue;
    if (find_symbol_name(i) != "?") { for (int j:hits) state[(size_t)j].flag=0; continue; }
    int found=0, class_=0, cnt16=0;
    for (int j:hits) {
      auto& e = state[(size_t)j];
      auto cr = routine_crc2(i, e.e->crc1); u32 y = cr.second; int cl=0;
      if (e.e->crc2 == y) cl=16;
      else { u32 yy = y ^ e.e->crc2; for (int k=0;k<32;k+=2) if (!(yy & (3u<<k))) cl++; }
      if (cl > class_) { class_=cl; if (class_==16) cnt16++; found=j; e.flag=2; }
      else e.flag=1;
    }
    if (class_ > 8) {
      int j = found;
      if (cnt16 > 1) for (int jj:hits) if (state[(size_t)jj].flag==2 && !state[(size_t)jj].fclass) { j=jj; break; }
      auto& e = state[(size_t)j];
      e.fclass = class_; e.found_at = i;
      e.symb = add_symbol(i, e.e->name, e.e->patch);
      this->found[i] = {e.e->name, e.e->patch};
      std::string nm = e.e->name;
      if (nm.find("EnqueueAndYield")!=std::string::npos || nm.find("YieldThread")!=std::string::npos)
        yield_addrs.insert(u32m(i | 0x80000000u));
    }
    for (int j:hits) state[(size_t)j].flag=0;
    if (i==0) break;
  }
  patch_names();
  int cnt=0, important=0, imp_found=0;
  for (auto& e: state) {
    if (e.found_at) cnt++;
    if (e.e->patch >= 10) { important++; if (e.found_at) imp_found++; }
  }
  last_report = "OS-routine search: " + std::to_string(cnt) + "/" + std::to_string(total) +
    " found, " + std::to_string(imp_found) + "/" + std::to_string(important) + " important";
  return last_report;
}
int UltraHleSym::add_patches() {
  int n=0; u32 first=0xFFFFFFFFu, last=0;
  for (auto& s : syms) {
    if (!s.patch) continue;
    u32 addr = s.addr | 0x80000000u;
    u32 old = _rd32(addr);
    if (_uh_op_op(old) != (int)ULTRAHLE_OP_PATCH) s.original = old;
    _wr32(addr, make_ultrahle_patch(s.patch));
    n++; u32 a = addr & ADDRMASK_UH;
    if (a < first) first=a; if (a > last) last=a;
  }
  patches_applied=n; first_patch = (first==0xFFFFFFFFu)?0:first; last_patch=last; return n;
}
std::string UltraHleSym::find_first_os() {
  std::string report;
  if (osrange0 && osrange1 && osrange1 > osrange0)
    report = find_os_calls(osrange0, osrange1 - osrange0);
  else
    report = find_os_calls(0x80000000u, 0x200000u);
  if ((int)this->found.size() < 8 && osrange0) {
    auto extra = find_os_calls(0x80000000u, 0x200000u);
    report += "; fallback=" + extra;
  }
  return report;
}
void UltraHleSym::apply_game_profile(const std::string& title) {
  ismario = iszelda = bootloader = 0;
  osrange0 = osrange1 = 0;
  ini_patches.clear();
  if (auto* p = ultrahle_find_profile(title)) {
    ismario = p->ismario; iszelda = p->iszelda; bootloader = p->bootloader;
    osrange0 = p->osrange0; osrange1 = p->osrange1;
    for (int i=0;i<p->npatches;i++) {
      auto& x = p->patches[i];
      ini_patches.emplace_back(x.when, x.addr, std::string(x.kind), x.data);
    }
  }
  std::string t = title; for (char& c:t) c=(char)toupper((unsigned char)c);
  if (t.rfind("SUPER MARIO",0)==0) { ismario=1; if (!osrange0) { osrange0=0x80300000u; osrange1=0x80380000u; } }
  if (t.rfind("THE LEGEND",0)==0 || t.find("ZELDA")!=std::string::npos) iszelda=1;
  if (t.find("BANJO")!=std::string::npos || t.rfind("F-ZERO",0)==0) bootloader=1;
}
int UltraHleSym::apply_ini_patches(int dma_num) {
  int n=0;
  for (auto& tup : ini_patches) {
    int when; u32 addr; std::string kind; u32 data;
    std::tie(when, addr, kind, data) = tup;
    if (when != dma_num && when != -1) continue;
    for (char& c: kind) c=(char)tolower((unsigned char)c);
    if (kind=="patch") { core->bus.write_u32(addr, make_ultrahle_patch((int)data)); n++; }
    else if (kind=="word") { core->bus.write_u32(addr, data); n++; }
    else if (kind=="byte") { core->bus.write_u8(addr, (u8)(data & 0xFF)); n++; }
  }
  return n;
}
std::string UltraHleSym::boot_scan_and_patch(const std::string& title, u32 codebase_in) {
  reset(); codebase = u32m(codebase_in);
  apply_game_profile(title);
  std::string report = find_first_os();
  int n = add_patches(); int ini_n = apply_ini_patches(0);
  report += "; patches=" + std::to_string(n) + " ini=" + std::to_string(ini_n) +
    " flags=m" + std::to_string(ismario) + "z" + std::to_string(iszelda) + "b" + std::to_string(bootloader) +
    " range=" + ([&]{ char b[32]; std::snprintf(b,sizeof(b),"%08X..%08X", first_patch, last_patch); return std::string(b);}());
  last_report = report; return report;
}


void ACsN64Core::reset() {
  std::fill(rdram.begin(), rdram.end(), 0);
  rsp_dmem.assign(RSP_DMEM_SIZE, 0); rsp_imem.assign(RSP_IMEM_SIZE, 0);
  pif_ram.assign(PIF_RAM_SIZE, 0); rsp_pc = 0;
  cpu.reset(); bus.reset(); ultrahle.reset(); uh_sym.reset();
  rdp.reset(); gfx.reset(); audio.reset(); jit.clear(); rsp.reset();
  running = false; frame_count = 0; cycle_count = 0; fb_lit = false; boot_turbo = true;
  fb_ppm.clear(); fb_rgba.clear(); fb_w=320; fb_h=240; unimpl.clear(); events.clear();
  cycles_per_frame = N64_CYCLES_PER_FRAME_NTSC; frame_period = FRAME_PERIOD_NTSC;
}


std::string ACsN64Core::load_rom(const std::string& path, bool lle_mode) {
  lle = lle_mode;
  try {
    auto raw = read_rom_file(path);
    if (raw.size() < 0x40) return "ROM too small (<64 bytes)";
    rom = normalize_rom_bytes(std::move(raw));
  } catch (const std::exception& e) { return e.what(); }
  rom_path = path;
  auto id = identify_cic(rom); cic = id.first;
  if (!id.second) note_unimpl("cic:unknown-ipl3");
  rom_header = N64Header(rom);
  seed_pif_ram(pif_ram, cic);
  region = get_rom_region(rom);
  if (region != REGION_PAL) { cycles_per_frame = N64_CYCLES_PER_FRAME_NTSC; frame_period = FRAME_PERIOD_NTSC; }
  else { cycles_per_frame = N64_CYCLES_PER_FRAME_PAL; frame_period = FRAME_PERIOD_PAL; }
  interp_steps = INTERP_BOOT_STEPS; fb_lit = false; boot_turbo = true;
  reset();
  bus.regs[PI_STATUS] = 0; bus.hw_interrupts = 0;
  _hle_ipl3_boot();
  audio.use_rom_tables(rom);
  return {};
}

void ACsN64Core::_hle_ipl3_boot() {
  auto& romv = rom;
  std::vector<u8> head(0x1000, 0);
  size_t ncopy = std::min(romv.size(), (size_t)0x1000);
  if (ncopy) std::memcpy(head.data(), romv.data(), ncopy);
  rsp_dmem = head; rsp_imem = head;
  u32 entry = normalize_commercial_entry(rom_header.boot_address);
  if (cic == CIC_NUS_6103) entry = u32m(entry - 0x100000);
  else if (cic == CIC_NUS_6106) entry = u32m(entry - 0x200000);
  auto prof = ultrahle_match_ini(rom_header.title);
  int bootloader = prof.count("bootloader") ? prof["bootloader"] : 0;
  if (romv.size() > 0x544 && be32(romv, 0x540) != 0) bootloader = 1;
  if (bootloader) entry = u32m(entry & ~0x300000u);
  u32 entry_phys = entry & 0x1FFFFFFF;
  std::fill(rdram.begin(), rdram.end(), 0);
  jit.clear();
  if (romv.size() > 0x1000 && entry_phys < RDRAM_SIZE) {
    size_t src_n = std::min(romv.size() - 0x1000, (size_t)0x100000);
    size_t n = std::min(src_n, (size_t)(RDRAM_SIZE - entry_phys));
    std::memcpy(rdram.data() + entry_phys, romv.data() + 0x1000, n);
  }
  auto& g = cpu.gpr;
  for (int i = 0; i < 32; ++i) g[i] = 0;
  g[1]=1; g[6]=0xFFFFFFFFA4001F0CULL; g[7]=0xFFFFFFFFA4001F08ULL; g[8]=0xC0; g[10]=0x40; g[11]=0xFFFFFFFFA4000040ULL; g[29]=0xFFFFFFFFA4001FF0ULL;
  if (cic==CIC_NUS_6101||cic==CIC_NUS_7102||cic==CIC_NUS_6102) { g[22]=0x3F; g[5]=0xFFFFFFFFC0F1D859ULL; g[14]=0x2DE108EA; }
  else if (cic==CIC_NUS_6103) { g[22]=0x78; g[5]=0xFFFFFFFFD4646273ULL; g[14]=0x1AF53600; }
  else if (cic==CIC_NUS_6105) { g[22]=0x91; g[5]=0xFFFFFFFFDECAAAD1ULL; g[14]=0x0CF85C13; }
  else if (cic==CIC_NUS_6106) { g[22]=0x85; g[5]=0xFFFFFFFFB04DC903ULL; g[14]=0x1AF53600; }
  else g[22]=0x3F;
  auto sit = CIC_SEEDS.find(cic); if (sit != CIC_SEEDS.end()) g[22] = sit->second;
  int region_ = get_rom_region(romv);
  int tv_type = region_==REGION_PAL?0:region_==REGION_MPAL?2:1;
  g[19]=0; g[20]=tv_type; g[21]=0; g[23]=0; g[24]=3; g[31]=0xFFFFFFFFA4001550ULL;
  cpu.hi=cpu.lo=0; cpu.llbit=false; cpu.lladdr=0;
  cpu.cp0[CP0_STATUS]=0x34000000|STATUS_CU1; cpu.cp0[CP0_CONFIG]=0x7006E463;
  cpu.cp0[CP0_COUNT]=0x5000; cpu.cp0[CP0_CAUSE]=0x5C; cpu.cp0[CP0_CONTEXT]=0x007FFFF0;
  cpu.cp0[CP0_EPC]=cpu.cp0[CP0_BADVADDR]=cpu.cp0[CP0_ERROREPC]=0xFFFFFFFF;
  cpu.cp0[CP0_COMPARE]=0xFFFFFFFF; cpu.cp0[CP0_PRID]=0x00000B00;
  u32 mem_size = (ram_mb==4?4:8)*1024*1024;
  put_be32(rdram,0x300,tv_type); put_be32(rdram,0x304,0); put_be32(rdram,0x308,0xB0000000);
  put_be32(rdram,0x30C,0); put_be32(rdram,0x314,0); put_be32(rdram,0x318,mem_size);
  if (cic==CIC_NUS_6105) {
    put_be32(rdram,0x3F0,mem_size);
    u32 stub[]={0x3C0DBFC0,0x8DA807FC,0x25AD07C0,0x31080080,0x5500FFFC,0x3C0DBFC0,0x8DA80024,0x3C0BB000};
    for (int i=0;i<8;i++) put_be32(rsp_imem,i*4,stub[i]);
  }
  cpu.pc=entry; cpu.next_pc=u32m(entry+4);
  save_mgr.save_type=detect_save_type(romv);
  ultrahle.reset(); bus.hw_interrupts=0; bus.mi_intr_mask=0; bus.sp_status=SP_STATUS_HALT; _vi_origin_set=false;
  if (lle) return;
  uh_boot_report = uh_sym.boot_scan_and_patch(rom_header.title, entry);
}

int ACsN64Core::step_frame(double budget_s) {
  if (!running) return 0;
  int steps_target = std::max(INTERP_MIN_STEPS, std::min(INTERP_MAX_STEPS, interp_steps));
  if (fixed_steps > 0) { steps_target = fixed_steps; budget_s = 1e9; }
  auto t0 = std::chrono::steady_clock::now();
  auto deadline = t0 + std::chrono::duration<double>(std::max(0.004, budget_s));
  auto cpu_deadline = t0 + std::chrono::duration<double>(std::max(0.003, budget_s * 0.85));
  int ran = 0;
  while (ran < steps_target && running) {
    cpu.step(); ++ran;
    if (fixed_steps <= 0 && (ran & 0xFF) == 0 && std::chrono::steady_clock::now() >= cpu_deadline) break;
  }
  cycle_count += ran;
  u64 catch_ = cycles_per_frame > (u64)ran ? cycles_per_frame - ran : 0;
  if (catch_) {
    u32 prev = cpu.cp0[CP0_COUNT];
    cpu.cp0[CP0_COUNT] = u32m(prev + catch_);
    u32 cmp = cpu.cp0[CP0_COMPARE];
    if (cmp && u32m(prev) < cmp && cmp <= cpu.cp0[CP0_COUNT]) cpu.cp0[CP0_CAUSE] |= CAUSE_IP7;
  }
  frame_count += 1;
  u32 vint = bus.regs.count(VI_INTR) ? (bus.regs[VI_INTR] & 0x3FF) : 0x3FF;
  bus.half_line = vint ? vint : 0x200;
  bus.hw_interrupts |= MI_INTR_VI;
  ultrahle.os_event(OS_EVENT_RETRACE);
  ultrahle.maybe_preempt();
  ultrahle.tick_time(cycles_per_frame);
  if (fixed_steps > 0 || std::chrono::steady_clock::now() < deadline || fb_ppm.empty())
    render_vi(fb_lit || fixed_steps > 0 ? 1 : 2);
  return ran;
}

bool ACsN64Core::render_vi(int scale) {
  if (scale < 1) scale = 1;
  u32 origin = (bus.regs.count(VI_ORIGIN) ? bus.regs[VI_ORIGIN] : 0) & 0xFFFFFF;
  int stride = (int)((bus.regs.count(VI_WIDTH) ? bus.regs[VI_WIDTH] : 320) & 0xFFF);
  if (stride < 16 || stride > 640) stride = 320;
  int width = stride, height = 240, bpp = 2;

  // Prefer programmed VI geometry (blanked VI → black, not RDRAM garbage / "static")
  u32 typ = (bus.regs.count(VI_STATUS) ? bus.regs[VI_STATUS] : 0) & 3;
  u32 hs = bus.regs.count(VI_H_START) ? bus.regs[VI_H_START] : 0;
  u32 vs = bus.regs.count(VI_V_START) ? bus.regs[VI_V_START] : 0;
  u32 hstart = (hs >> 16) & 0x3FF, hend = hs & 0x3FF;
  u32 vstart = (vs >> 16) & 0x3FF, vend = vs & 0x3FF;
  bool vi_blank = (typ < 2) || (hend <= hstart) || (vend <= vstart) || origin == 0;
  if (!vi_blank) {
    u32 xs = (bus.regs.count(VI_X_SCALE) ? bus.regs[VI_X_SCALE] : 0) & 0xFFF;
    u32 ys = (bus.regs.count(VI_Y_SCALE) ? bus.regs[VI_Y_SCALE] : 0) & 0xFFF;
    if (xs) width = (int)(((hend - hstart) * xs + 1023) >> 10);
    if (ys) height = (int)(((((vend - vstart) >> 1) * ys) + 1023) >> 10);
    width = std::max(1, std::min(width, std::min(stride, 640)));
    height = std::max(1, std::min(height, 480));
    bpp = (typ == 3) ? 4 : 2;
  }

  int out_w = std::max(1, width / scale);
  int out_h = std::max(1, height / scale);
  fb_w = out_w; fb_h = out_h;
  fb_rgba.assign((size_t)out_w * (size_t)out_h * 4, 0);

  // Present when lit, or when SoftRDP has a ready color image matching VI (never raw RDRAM snow).
  bool origin_is_cimg = rdp.cimg != 0 && ((rdp.cimg & 0xFFFFFF) == origin);
  bool allow_scan = !vi_blank && (fb_lit || (rdp.fb_ready && origin_is_cimg) ||
                                 (rdp.pixels_drawn > 1000 && origin_is_cimg));
  if (!allow_scan) {
    std::string hdr = "P6\n" + std::to_string(out_w) + " " + std::to_string(out_h) + "\n255\n";
    fb_ppm.assign(hdr.begin(), hdr.end());
    fb_ppm.resize(fb_ppm.size() + (size_t)out_w * (size_t)out_h * 3, 0);
    return false;
  }

  u64 bright = 0, nonzero = 0, variance = 0; size_t samples = 0;
  u8 prev = 0;
  for (int y = 0; y < out_h; y++) {
    for (int x = 0; x < out_w; x++) {
      int sx = x * scale, sy = y * scale;
      u8 r=0,g=0,b=0;
      if (bpp == 2) {
        u32 off = origin + (u32)((sy * stride + sx) * 2);
        if (off + 1 < rdram.size()) {
          u16 pix = (u16)((rdram[off] << 8) | rdram[off + 1]);
          r = (u8)(((pix >> 11) & 0x1F) * 255 / 31);
          g = (u8)(((pix >> 6) & 0x1F) * 255 / 31);
          b = (u8)(((pix >> 1) & 0x1F) * 255 / 31);
        }
      } else {
        u32 off = origin + (u32)((sy * stride + sx) * 4);
        if (off + 2 < rdram.size()) { r = rdram[off]; g = rdram[off+1]; b = rdram[off+2]; }
      }
      size_t di = ((size_t)y * (size_t)out_w + (size_t)x) * 4;
      fb_rgba[di]=r; fb_rgba[di+1]=g; fb_rgba[di+2]=b; fb_rgba[di+3]=255;
      u8 lum = (u8)((r*3 + g*6 + b)/10);
      bright += lum; nonzero += (lum > 8) ? 1 : 0;
      if (samples) variance += (u64)std::abs((int)lum - (int)prev);
      prev = lum; samples++;
    }
  }
  std::string hdr = "P6\n" + std::to_string(out_w) + " " + std::to_string(out_h) + "\n255\n";
  fb_ppm.assign(hdr.begin(), hdr.end());
  fb_ppm.resize(fb_ppm.size() + (size_t)out_w * (size_t)out_h * 3);
  for (int i = 0; i < out_w * out_h; i++) {
    fb_ppm[hdr.size() + (size_t)i*3 + 0] = fb_rgba[(size_t)i*4 + 0];
    fb_ppm[hdr.size() + (size_t)i*3 + 1] = fb_rgba[(size_t)i*4 + 1];
    fb_ppm[hdr.size() + (size_t)i*3 + 2] = fb_rgba[(size_t)i*4 + 2];
  }
  // Reject snow/static: ready RDP buffer or stable non-noise image
  if (!fb_lit && samples) {
    double avg = (double)bright / (double)samples;
    double nz = (double)nonzero / (double)samples;
    double var = (double)variance / (double)samples;
    bool coherent = (rdp.fb_ready && origin_is_cimg) ||
                    (rdp.pixels_drawn > 2000 && origin_is_cimg) ||
                    (avg >= 8.0 && avg <= 220.0 && nz >= 0.02 && nz <= 0.98 && var < 40.0);
    if (coherent) { fb_lit = true; boot_turbo = false; }
    else {
      std::fill(fb_rgba.begin(), fb_rgba.end(), 0);
      for (size_t i = 0; i < (size_t)out_w * (size_t)out_h; i++) fb_rgba[i*4+3] = 255;
    }
  }
  return true;
}

void SoftRDP::clear_cimg(u16 color) {
  if (!core || !cimg || !cwidth) return;
  fill_rect(0, 0, cwidth, 240, color, true);
  fb_ready = true;
}

void SoftRDP::present_buffer(u32 phys, int width, int height, u16 color) {
  if (!core || !phys) return;
  if (width < 16) width = 320;
  if (height < 1) height = 240;
  cimg = phys & 0xFFFFFF;
  cwidth = width;
  fill_rect(0, 0, (u32)width, (u32)height, color, true);
  fb_ready = true;
}

void SoftRDP::fill_rect(u32 x0, u32 y0, u32 x1, u32 y1, u16 color, bool count_pixels) {
  if (!core || !cimg) return;
  u32 w = cwidth ? cwidth : 320;
  if (x1 < x0) std::swap(x0, x1);
  if (y1 < y0) std::swap(y0, y1);
  if (x1 > w) x1 = w;
  if (y1 > 480) y1 = 480;
  for (u32 y = y0; y < y1; y++) {
    for (u32 x = x0; x < x1; x++) {
      u32 off = (cimg & 0xFFFFFF) + (y * w + x) * 2;
      if (off + 1 < core->rdram.size()) {
        core->rdram[off] = (u8)(color >> 8);
        core->rdram[off + 1] = (u8)(color & 0xFF);
        if (count_pixels) { stats["pixels"]++; pixels_drawn++; }
      }
    }
  }
  if (count_pixels && pixels_drawn) fb_ready = true;
}

u32 GfxHLE::resolve(u32 addr) const {
  // F3D segmented address: seg[31:24] + offset[23:0], base from gSPSegment
  u32 seg = (addr >> 24) & 0x0F;
  u32 off = addr & 0x00FFFFFF;
  if (seg && segments[seg]) return (segments[seg] + off) & 0x1FFFFFFF;
  return addr & 0x1FFFFFFF;
}

void GfxHLE::walk_dl(u32 ptr, int depth) {
  if (!core || !rdp || depth > 16) return;
  ptr &= 0x1FFFFFFF;
  rdp->stats["dls"]++;
  for (int n = 0; n < 0x8000 && ptr + 7 < core->rdram.size(); n++, ptr += 8) {
    u32 w0 = be32(core->rdram, ptr);
    u32 w1 = be32(core->rdram, ptr + 4);
    u8 cmd = (u8)(w0 >> 24);
    rdp->stats["cmds"]++;
    if (cmd == 0xB8 || cmd == 0xDF) break; // G_ENDDL
    if (cmd == 0x00 || cmd == 0xC0) continue; // G_SPNOOP / G_NOOP
    if (cmd == 0x06) { // G_DL — branch/call nested display list
      u32 next = resolve(w1);
      bool push = ((w0 >> 16) & 0xFF) == 0; // 0=push, 1=branch
      walk_dl(next, depth + 1);
      if (!push) break;
      continue;
    }
    if (cmd == 0xBC) { // G_MOVEWORD — segments (G_MW_SEGMENT=6)
      u8 idx = (u8)((w0 >> 16) & 0xFF);
      u16 off = (u16)(w0 & 0xFFFF);
      if (idx == 6 && (off & 3) == 0) {
        u32 seg = (off >> 2) & 0xF;
        segments[seg] = w1 & 0x1FFFFFFF;
      }
      continue;
    }
    if (cmd == 0xFF || cmd == 0x3D) { // G_SETCIMG
      rdp->cimg = resolve(w1) & 0x00FFFFFF;
      rdp->cwidth = (int)((w0 & 0xFFF) + 1);
      rdp->cfmt = (w0 >> 21) & 7;
      rdp->csiz = (w0 >> 19) & 3;
      if (!rdp->cwidth || rdp->cwidth > 640) rdp->cwidth = 320;
      rdp->clear_cimg(rdp->fill_color ? rdp->fill_color : (u16)0x0001);
    } else if (cmd == 0xFE) {
      // G_SETZIMG — ignore depth image for SoftRDP
    } else if (cmd == 0xF7 || cmd == 0x37) { // G_SETFILLCOLOR
      rdp->fill_color = (u16)((w1 >> 16) & 0xFFFF);
      if (!rdp->fill_color) rdp->fill_color = (u16)(w1 & 0xFFFF);
    } else if (cmd == 0xF6 || cmd == 0x36) { // G_FILLRECT
      u32 x1 = (w0 >> 12) & 0xFFF, y1 = w0 & 0xFFF;
      u32 x0 = (w1 >> 12) & 0xFFF, y0 = w1 & 0xFFF;
      rdp->fill_rect(x0 >> 2, y0 >> 2, x1 >> 2, y1 >> 2, rdp->fill_color);
    } else if (cmd == 0xE4 || cmd == 0xE5) { // G_TEXRECT / FLIP — soft fill placeholder
      u32 x0 = (w1 >> 12) & 0xFFF, y0 = w1 & 0xFFF;
      u32 x1 = (w0 >> 12) & 0xFFF, y1 = w0 & 0xFFF;
      rdp->fill_rect(x0 >> 2, y0 >> 2, x1 >> 2, y1 >> 2,
                     rdp->fill_color ? rdp->fill_color : (u16)0x07C1);
      rdp->stats["tris"]++;
    } else if (cmd == 0xBF || cmd == 0xB5 || cmd == 0xB4) {
      rdp->stats["tris"]++;
    } else if (cmd == 0xE9) { // G_RDPFULLSYNC
      if (rdp->cimg) rdp->fb_ready = true;
    }
  }
}

void GfxHLE::run_task(u32 data_ptr, const std::string&) {
  if (!core || !rdp || !data_ptr) return;
  // F3D / F3DEX GBI walker (Project64-Legacy plugin-equivalent HLE path)
  walk_dl(resolve(data_ptr), 0);
}

void ACsN64Core::process_rdp(u32 data_ptr, u32, u32, u32) {
  bus.dpc_status &= ~DPC_STATUS_FREEZE;
  u32 task = last_os_task;
  if (!data_ptr && task) data_ptr = bus.read_u32(task + 0x30);
  if (data_ptr) gfx.run_task(data_ptr);
  bus.hw_interrupts |= MI_INTR_DP;
  if (rdp.fb_ready || rdp.pixels_drawn > 0) {
    // If VI origin still empty but we have a color image, point VI at it (boot assist)
    u32 vo = (bus.regs.count(VI_ORIGIN) ? bus.regs[VI_ORIGIN] : 0) & 0xFFFFFF;
    if (!vo && rdp.cimg) {
      bus.regs[VI_ORIGIN] = rdp.cimg & 0xFFFFFF;
      if (!bus.regs.count(VI_WIDTH) || !(bus.regs[VI_WIDTH] & 0xFFF))
        bus.regs[VI_WIDTH] = rdp.cwidth ? (u32)rdp.cwidth : 320;
      if (!bus.regs.count(VI_STATUS) || (bus.regs[VI_STATUS] & 3) < 2)
        bus.regs[VI_STATUS] = 0x320E; // 16-bit RGBA, typical early boot
      if (!bus.regs.count(VI_H_START)) bus.regs[VI_H_START] = 0x006C02EC;
      if (!bus.regs.count(VI_V_START)) bus.regs[VI_V_START] = 0x002501FF;
      if (!bus.regs.count(VI_X_SCALE)) bus.regs[VI_X_SCALE] = 0x200;
      if (!bus.regs.count(VI_Y_SCALE)) bus.regs[VI_Y_SCALE] = 0x400;
      _vi_origin_set = true;
    }
    render_vi(1);
  }
}

void ACsN64Core::process_rsp() {
  u32 task = last_os_task;
  u32 task_type = 0;
  if (task) {
    task_type = bus.read_u32(task);
    put_be32(rsp_dmem, 0xFC0, task_type);
  } else if (rsp_dmem.size() >= 0xFC4) {
    task_type = be32(rsp_dmem, 0xFC0);
  }
  bus.sp_status |= SP_STATUS_HALT | SP_STATUS_BROKE;
  bus.sp_status &= ~(SP_STATUS_DMA_BUSY | SP_STATUS_DMA_FULL | SP_STATUS_IO_FULL);
  bus.hw_interrupts |= MI_INTR_SP;
  if (task_type == 1) {
    u32 data_ptr = task ? bus.read_u32(task + 0x30) : be32(rsp_dmem, 0xFF0);
    process_rdp(data_ptr);
  } else if (task_type == 2) {
    process_audio_task(be32(rsp_dmem, 0xFF0), be32(rsp_dmem, 0xFF4));
  } else {
    note_unimpl("rsp:task" + std::to_string(task_type));
    bus.hw_interrupts |= MI_INTR_DP;
  }
}

void ACsN64Core::trigger_sp_dma(bool to_rsp) {
  u32 reg = bus.regs.count(to_rsp ? SP_RD_LEN : SP_WR_LEN) ? bus.regs[to_rsp ? SP_RD_LEN : SP_WR_LEN] : 0;
  int length = (int)(((reg & 0xFFF) | 7) + 1);
  int count = (int)(((reg >> 12) & 0xFF) + 1);
  int skip = (int)((reg >> 20) & 0xFFF);
  u32 sp_mem = (bus.regs.count(SP_MEM_ADDR) ? bus.regs[SP_MEM_ADDR] : 0) & 0x1FF8;
  u32 dram = (bus.regs.count(SP_DRAM_ADDR) ? bus.regs[SP_DRAM_ADDR] : 0) & 0x00FFFFF8;
  bool imem = (sp_mem & 0x1000) != 0;
  auto& mem = imem ? rsp_imem : rsp_dmem;
  u32 off = sp_mem & 0xFF8;
  for (int c = 0; c < count; c++) {
    for (int k = 0; k < length; k += 8) {
      u32 o = (off + (u32)k) & 0xFF8;
      u32 d = dram + (u32)k;
      if (d + 8 > rdram.size()) break;
      if (to_rsp) std::memcpy(mem.data() + o, rdram.data() + d, 8);
      else std::memcpy(rdram.data() + d, mem.data() + o, 8);
    }
    if (!to_rsp) jit.invalidate_range(dram, (u32)length);
    off = (off + (u32)length) & 0xFF8;
    dram += (u32)(length + skip);
  }
  bus.regs[SP_MEM_ADDR] = (sp_mem & 0x1000) | off;
  bus.regs[SP_DRAM_ADDR] = dram & 0xFFFFF8;
  if (!lle) bus.hw_interrupts |= MI_INTR_SP;
}

void ACsN64Core::trigger_pi_dma() {
  u32 dram = (bus.regs.count(PI_DRAM_ADDR) ? bus.regs[PI_DRAM_ADDR] : 0) & 0x00FFFFFF;
  u32 cart = (bus.regs.count(PI_CART_ADDR) ? bus.regs[PI_CART_ADDR] : 0);
  u32 rdlen = (bus.regs.count(PI_RD_LEN) ? bus.regs[PI_RD_LEN] : 0) & 0x00FFFFFF;
  u32 wrlen = (bus.regs.count(PI_WR_LEN) ? bus.regs[PI_WR_LEN] : 0) & 0x00FFFFFF;
  bool cart_to_rdram = wrlen > 0 || rdlen == 0;
  u32 length = wrlen ? wrlen : rdlen;
  length = ((length & 0x00FFFFFF) + 2) & ~1u;
  u32 ca = cart & 0x1FFFFFFF;
  if (ca < 0x04000000u) { ca = 0x10000000u + ca; cart = ca; }
  if (cart_to_rdram) {
    if (ca >= 0x10000000u && ca < 0x10000000u + rom.size()) {
      u32 off = ca - 0x10000000u;
      u32 n = (u32)std::min((size_t)length, rom.size() - off);
      n = (u32)std::min((size_t)n, rdram.size() - dram);
      if (n) { std::memcpy(rdram.data() + dram, rom.data() + off, n); jit.invalidate_range(dram, n); }
    } else if (ca >= 0x08000000u && ca < 0x09000000u) {
      save_mgr.pi_read(ca, (int)length, rdram, dram);
      jit.invalidate_range(dram, length);
    }
  } else {
    save_mgr.pi_write(ca, (int)length, rdram, dram);
  }
  bus.regs[PI_WR_LEN] = 0; bus.regs[PI_RD_LEN] = 0;
  bus.hw_interrupts |= MI_INTR_PI;
}

void ACsN64Core::trigger_si_dma(bool read_pif) {
  u32 dram = (bus.regs.count(SI_DRAM_ADDR) ? bus.regs[SI_DRAM_ADDR] : 0) & 0x00FFFFFF;
  if (read_pif) {
    _pif_process_commands();
    for (size_t i = 0; i < PIF_RAM_SIZE && dram + i < rdram.size(); i++) rdram[dram + i] = pif_ram[i];
  } else {
    for (size_t i = 0; i < PIF_RAM_SIZE && dram + i < rdram.size(); i++) pif_ram[i] = rdram[dram + i];
    _pif_process_commands();
  }
  bus.hw_interrupts |= MI_INTR_SI;
}
bool ACsN64Core::_pif_control() {
  u8 ctl = pif_ram[0x3F];
  if (ctl & 0x02) {
    std::vector<int> chl;
    for (int i=0;i<15;i++) { chl.push_back((pif_ram[0x30+i]>>4)&0xF); chl.push_back(pif_ram[0x30+i]&0xF); }
    auto rsp = cic_6105_response(std::vector<int>(chl.begin(), chl.begin()+30));
    pif_ram[0x2E]=pif_ram[0x2F]=0;
    for (int i=0;i<15;i++) pif_ram[0x30+i]=(u8)((rsp[i*2]<<4)|rsp[i*2+1]);
    pif_ram[0x3F]=0; return true;
  }
  if (ctl & 0x08) { pif_ram[0x3F]=0; return true; }
  return false;
}
void ACsN64Core::_pif_process_commands() {
  if (_pif_control()) return;
  pif_ram[0x3F]=0;
}

// ── Opcode listing / assert ──
static std::vector<std::string> list_implemented_opcodes() {
  std::vector<std::string> names;
  for (auto& p : PRIMARY_OPS) {
    if (p.reserved) continue;
    std::string name=p.name;
    if (name=="SPECIAL") {
      for (auto& s : SPECIAL_OPS) if (!s.reserved && _DISPATCH[_ID_SPECIAL|s.code]) names.push_back(s.name);
    } else if (name=="REGIMM") {
      for (auto& s : REGIMM_OPS) if (!s.reserved && _DISPATCH[_ID_REGIMM|s.code]) names.push_back(s.name);
    } else if (name=="COP0") {
      for (auto& s : COP0_RS_OPS) if (std::string(s.name)!="COP0_CO" && _DISPATCH[_ID_COP0_RS|s.code]) names.push_back(s.name);
      for (auto& s : COP0_CO_OPS) if (_DISPATCH[_ID_COP0_CO|s.code]) names.push_back(s.name);
    } else if (name=="COP1") {
      for (auto& s : COP1_RS_OPS) {
        std::string n=s.name;
        if (n=="S"||n=="D"||n=="W"||n=="L") {
          for (int code : COP1_FUNCT_CODES) {
            const char* fn = nullptr;
            // map code to name
            static const std::unordered_map<int,const char*> M = {
              {0x00,"ADD"},{0x01,"SUB"},{0x02,"MUL"},{0x03,"DIV"},{0x04,"SQRT"},{0x05,"ABS"},{0x06,"MOV"},{0x07,"NEG"},
              {0x08,"ROUND.L"},{0x09,"TRUNC.L"},{0x0A,"CEIL.L"},{0x0B,"FLOOR.L"},{0x0C,"ROUND.W"},{0x0D,"TRUNC.W"},{0x0E,"CEIL.W"},{0x0F,"FLOOR.W"},
              {0x15,"RECIP"},{0x16,"RSQRT"},{0x20,"CVT.S"},{0x21,"CVT.D"},{0x24,"CVT.W"},{0x25,"CVT.L"},
              {0x30,"C.F"},{0x31,"C.UN"},{0x32,"C.EQ"},{0x33,"C.UEQ"},{0x34,"C.OLT"},{0x35,"C.ULT"},{0x36,"C.OLE"},{0x37,"C.ULE"},
              {0x38,"C.SF"},{0x39,"C.NGLE"},{0x3A,"C.SEQ"},{0x3B,"C.NGL"},{0x3C,"C.LT"},{0x3D,"C.NGE"},{0x3E,"C.LE"},{0x3F,"C.NGT"},
            };
            auto it=M.find(code); if(it!=M.end()) names.push_back(std::string(it->second)+"."+n);
          }
        } else if (n=="BC1") names.push_back("BC1");
        else if (_DISPATCH[_ID_COP1_RS|s.code]) names.push_back(n);
      }
    } else if (name=="COP2") {
      for (auto& s : COP2_RS_OPS) if (_DISPATCH[_ID_PRIMARY|0x12]) names.push_back(s.name);
    } else if (_DISPATCH[_ID_PRIMARY|p.code]) names.push_back(name);
  }
  if (_DISPATCH[_ID_COP1_BC|0]) { if (std::find(names.begin(),names.end(),"BC1")==names.end()) names.push_back("BC1"); }
  std::sort(names.begin(), names.end());
  names.erase(std::unique(names.begin(), names.end()), names.end());
  return names;
}

static void assert_opcode_table() {
  std::vector<std::string> missing;
  for (auto& p : PRIMARY_OPS) {
    std::string name=p.name;
    if (name=="SPECIAL"||name=="REGIMM"||name=="COP0"||name=="COP1") continue;
    if (p.reserved) { if (!_DISPATCH[_ID_PRIMARY|p.code]) missing.push_back("PRIMARY."+name); continue; }
    if (!_DISPATCH[_ID_PRIMARY|p.code]) missing.push_back(name);
  }
  for (auto& s : SPECIAL_OPS) if (!_DISPATCH[_ID_SPECIAL|s.code]) missing.push_back(std::string("SPECIAL.")+s.name);
  for (auto& s : REGIMM_OPS) if (!_DISPATCH[_ID_REGIMM|s.code]) missing.push_back(std::string("REGIMM.")+s.name);
  for (int f = 0; f < 64; ++f) if (!_DISPATCH[_ID_SPECIAL|f]) missing.push_back("SPECIAL.slot"+std::to_string(f));
  for (int rt = 0; rt < 32; ++rt) if (!_DISPATCH[_ID_REGIMM|rt]) missing.push_back("REGIMM.slot"+std::to_string(rt));
  for (int rs = 0; rs < 32; ++rs) if (!_DISPATCH[_ID_COP0_RS|rs]) missing.push_back("COP0.RS"+std::to_string(rs));
  for (int f = 0; f < 64; ++f) if (!_DISPATCH[_ID_COP0_CO|f]) missing.push_back("COP0.CO"+std::to_string(f));
  for (auto& s : COP0_RS_OPS) if (std::string(s.name)!="COP0_CO" && !_DISPATCH[_ID_COP0_RS|s.code]) missing.push_back(std::string("COP0.")+s.name);
  for (auto& s : COP0_CO_OPS) if (!_DISPATCH[_ID_COP0_CO|s.code]) missing.push_back(std::string("COP0.")+s.name);
  for (auto& s : COP1_RS_OPS) {
    std::string n=s.name; if (n=="S"||n=="D"||n=="W"||n=="L"||n=="BC1") continue;
    if (!_DISPATCH[_ID_COP1_RS|s.code]) missing.push_back("COP1."+n);
  }
  for (int rs = 0; rs < 32; ++rs) if (!_DISPATCH[_ID_COP1_RS|rs]) missing.push_back("COP1.RS"+std::to_string(rs));
  for (int rt=0; rt<32; ++rt) if (!_DISPATCH[_ID_COP1_BC|rt]) missing.push_back("BC1.rt"+std::to_string(rt));
  for (int fid : {_ID_FPU_S,_ID_FPU_D,_ID_FPU_W,_ID_FPU_L})
    for (int f = 0; f < 64; ++f)
      if (!_DISPATCH[_ID_FPU|(fid<<6)|f]) missing.push_back("FPU");
  if (!missing.empty()) throw std::runtime_error("Missing dispatch handlers: "+missing[0]+" (+" + std::to_string(missing.size()-1)+")");
}

#define CHECK(cond) do { if (!(cond)) { std::cerr << "FAIL: " << #cond << " at " << __LINE__ << "\n"; return 1; } } while(0)

static int self_test() {
  init_dispatch();
  init_ultrahle_tables();
  assert_opcode_table();
  auto names = list_implemented_opcodes();
  ACsN64Core core;
  auto& cpu = core.cpu;
  auto* g = cpu.gpr;
  cpu.execute(N64Opcode(0x3C010123)); CHECK((g[1] & MASK_32) == 0x01230000);
  cpu.execute(N64Opcode(0x34215678)); CHECK((g[1] & MASK_32) == 0x01235678);
  g[2]=5; g[3]=7;
  cpu.execute(N64Opcode(0x00431021)); CHECK(sign32(g[2]) == 12);
  // DIV / DDIV MIPS edge cases (div-by-zero + INT_MIN/-1)
  g[4]=sx32_to_64(100); g[5]=0;
  cpu.execute(N64Opcode(0x0085001A)); // DIV $4,$5
  CHECK(cpu.lo == MASK_64 && sign32(cpu.hi) == 100);
  g[4]=sx32_to_64((s32)0x80000000); g[5]=sx32_to_64(-1);
  cpu.execute(N64Opcode(0x0085001A));
  CHECK(sign32(cpu.lo) == (s32)0x80000000 && cpu.hi == 0);
  g[6]=7; g[7]=3;
  cpu.execute(N64Opcode(0x00C7001A)); // DIV 7/3
  CHECK(sign32(cpu.lo) == 2 && sign32(cpu.hi) == 1);
  // Unaligned LWL/LWR merge (big-endian VR4300)
  put_be32(core.rdram, 0x100, 0xAABBCCDD);
  g[8]=0x80000100; g[9]=0;
  cpu.execute(N64Opcode(0x89090002)); // LWL $9,2($8)
  CHECK((g[9] & MASK_32) == 0xCCDD0000u);
  g[9]=0x11223344;
  cpu.execute(N64Opcode(0x99090002)); // LWR $9,2($8)
  CHECK((g[9] & MASK_32) == 0x11AABBCCu);
  // COP0 CO bit decode (ERET via rs=0x10)
  CHECK(N64Opcode(0x42000018).instr_id == (_ID_COP0_CO | 0x18));
  CHECK(N64Opcode(0x40085800).instr_id == (_ID_COP0_RS | 0x00)); // MFC0
  cpu.cp0[CP0_STATUS] |= STATUS_CU1;
  cpu.fpr[0] = 4;
  cpu.execute(N64Opcode(0x46800020)); // CVT.S.W
  cpu.fcr31 |= (1u << FCR31_COND_BIT);
  u32 old_pc = cpu.pc; cpu.next_pc = u32m(old_pc + 4);
  cpu.execute(N64Opcode(0x45010004)); // BC1T
  bool has_recip=false, has_rsqrt=false;
  for (auto& n : names) { if (n.rfind("RECIP",0)==0) has_recip=true; if (n.rfind("RSQRT",0)==0) has_rsqrt=true; }
  CHECK(has_recip && has_rsqrt);
  CHECK(std::find(names.begin(),names.end(),"ERET")!=names.end());
  CHECK(std::find(names.begin(),names.end(),"CACHE")!=names.end());
  CHECK(std::find(names.begin(),names.end(),"SYNC")!=names.end());
  CHECK(std::find(names.begin(),names.end(),"WAIT")!=names.end());
  CHECK(std::find(names.begin(),names.end(),"PATCH")!=names.end());
  CHECK(std::find(names.begin(),names.end(),"GROUP")!=names.end());
  CHECK(std::find(names.begin(),names.end(),"MFC2")!=names.end());
  CHECK(std::find(names.begin(),names.end(),"DCFC1")!=names.end());
  // SoftRDP present path (PJ64 plugin-style VI swap) unlocks lit frame without snow
  {
    ACsN64Core gfxc;
    gfxc.rdp.present_buffer(0x1000, 320, 240, 0x0C3F);
    gfxc.bus.regs[VI_ORIGIN] = 0x1000;
    gfxc.bus.regs[VI_WIDTH] = 320;
    gfxc.bus.regs[VI_STATUS] = 0x320E;
    gfxc.bus.regs[VI_H_START] = 0x006C02EC;
    gfxc.bus.regs[VI_V_START] = 0x002501FF;
    gfxc.bus.regs[VI_X_SCALE] = 0x200;
    gfxc.bus.regs[VI_Y_SCALE] = 0x400;
    gfxc.fb_lit = true;
    CHECK(gfxc.render_vi(1));
    CHECK(gfxc.rdp.fb_ready && gfxc.rdp.pixels_drawn > 1000);
    CHECK(!gfxc.fb_rgba.empty());
  }
  // F3D segment resolve + nested DL
  {
    ACsN64Core gfxc;
    gfxc.gfx.segments[7] = 0x2000;
    put_be32(gfxc.rdram, 0x2000, 0xFF10013F); // SETCIMG siz/fmt/width-1
    put_be32(gfxc.rdram, 0x2004, 0x00100000);
    put_be32(gfxc.rdram, 0x2008, 0xF7000000); // SETFILLCOLOR
    put_be32(gfxc.rdram, 0x200C, 0x07C107C1);
    put_be32(gfxc.rdram, 0x2010, 0xF641C23F); // FILLRECT roughly full
    put_be32(gfxc.rdram, 0x2014, 0x00000000);
    put_be32(gfxc.rdram, 0x2018, 0xB8000000); // ENDDL
    put_be32(gfxc.rdram, 0x201C, 0x00000000);
    gfxc.gfx.run_task(0x07000000);
    CHECK(gfxc.rdp.cimg == 0x100000);
    CHECK(gfxc.rdp.fb_ready);
  }
  // Project64-Legacy integer overflow (ADD)
  g[2]=sx32_to_64(0x7FFFFFFF); g[3]=sx32_to_64(1); g[4]=0;
  u32 ov_pc = 0x80004000; cpu.pc = ov_pc; cpu.next_pc = ov_pc + 4;
  cpu.cp0[CP0_STATUS] &= ~(STATUS_EXL|STATUS_ERL); cpu.cp0[CP0_STATUS] |= STATUS_IE;
  cpu.execute(N64Opcode(0x00432020)); // ADD $4,$2,$3 — overflow
  CHECK(cpu.pc == 0x80000180); // general exception vector
  cpu.pc = 0x80000000; cpu.next_pc = 0x80000004; cpu.cp0[CP0_STATUS] &= ~STATUS_EXL;
  // COP2 latch (with CU2)
  cpu.cp0[CP0_STATUS] |= STATUS_CU2; cpu.cop2_latch = 0;
  g[5]=0x12345678; cpu.execute(N64Opcode(0x48850000)); // MTC2 $5,$0 (COP2 rs=4)
  g[6]=0; cpu.execute(N64Opcode(0x48060000)); // MFC2 $6,$0 (COP2 rs=0)
  CHECK((g[6] & MASK_32) == 0x12345678u);
  CHECK(ULTRAHLE_PATCH_TABLE.size() >= 61);
  CHECK(ULTRAHLE_PATCH_TABLE.size() == ULTRAHLE_PATCH_NAMES.size());
  for (auto& [k,v] : ULTRAHLE_PATCH_TABLE) CHECK(ULTRAHLE_PATCH_NAMES.count(k));
  g[_UH_A0]=0; g[_UH_A1]=6; g[_UH_A2]=0; g[_UH_A3]=7; g[_UH_RA]=0x80001000;
  cpu.pc=0x80000000; cpu.next_pc=0x80000004;
  cpu.execute(N64Opcode(make_ultrahle_patch(4)));
  CHECK(g[_UH_V0]==0 && g[_UH_V1]==42);
  CHECK(cpu.pc==0x80001000 && cpu.next_pc==0x80001004);
  static const u8 pat56[8] = {0x11,0x22,0x33,0x44,0x55,0x66,0x77,0x88};
  for (int i=0;i<8;i++) core.rdram[i] = pat56[i];
  g[_UH_A0]=0x80000010; g[_UH_A1]=0x80000000; g[_UH_A2]=8; g[_UH_RA]=0x80002000;
  cpu.pc=0x80000000; cpu.next_pc=0x80000004;
  cpu.execute(N64Opcode(make_ultrahle_patch(56)));
  CHECK(std::memcmp(core.rdram.data()+0x10, "\x11\x22\x33\x44\x55\x66\x77\x88", 8)==0);
  core.save_mgr.save_type = SAVE_EEPROM_4K;
  core.save_mgr.eeprom.assign(EEPROM_16K_SIZE, 0);
  g[_UH_RA]=0x80003000; cpu.pc=0x80000000; cpu.next_pc=0x80000004;
  cpu.execute(N64Opcode(make_ultrahle_patch(57)));
  CHECK(g[_UH_V0]==EEPROM_TYPE_4K);
  static const u8 pat_ee[8] = {0xDE,0xAD,0xBE,0xEF,0xCA,0xFE,0xF0,0x0D};
  for (int i=0;i<8;i++) core.rdram[0x40+i]=pat_ee[i];
  g[_UH_A1]=3; g[_UH_A2]=0x80000040; g[_UH_RA]=0x80003000;
  cpu.pc=0x80000000; cpu.next_pc=0x80000004;
  cpu.execute(N64Opcode(make_ultrahle_patch(59)));
  CHECK(g[_UH_V0]==0);
  std::memset(core.rdram.data()+0x50, 0, 8);
  g[_UH_A1]=3; g[_UH_A2]=0x80000050; g[_UH_RA]=0x80003000;
  cpu.pc=0x80000000; cpu.next_pc=0x80000004;
  cpu.execute(N64Opcode(make_ultrahle_patch(58)));
  CHECK(g[_UH_V0]==0);
  CHECK(std::memcmp(core.rdram.data()+0x50, "\xDE\xAD\xBE\xEF\xCA\xFE\xF0\x0D", 8)==0);
  cpu.pc=0x80000000; cpu.next_pc=0x80000004;
  cpu.execute(N64Opcode(u32m((ULTRAHLE_OP_GROUP<<26)|1)));
  CHECK(cpu.pc==0x80000004);
  core.ultrahle.install_patch(0x80000100, 23);
  CHECK(core.bus.read_u32(0x80000100)==make_ultrahle_patch(23));
  CHECK(ULTRAHLE_OSCALL_N >= 200);
  CHECK(ULTRAHLE_OSPATCH_N >= 50);
  bool disable_has_pi=false;
  for (size_t i=0;i<ULTRAHLE_DISABLE_PATCHES_N;i++) if (std::string(ULTRAHLE_DISABLE_PATCHES[i])=="osPiStartDma") disable_has_pi=true;
  CHECK(!disable_has_pi);
  CHECK(ultrahle_match_ini("SUPER MARIO 64")["ismario"]==1);
  CHECK(ultrahle_match_ini("Banjo-Kazooie")["bootloader"]==1);
  bool eeprom_probe=false;
  for (size_t i=0;i<ULTRAHLE_OSCALL_N;i++)
    if (ULTRAHLE_OSCALL[i].patch==57 && std::string(ULTRAHLE_OSCALL[i].name).find("osEepromProbe")!=std::string::npos) eeprom_probe=true;
  CHECK(eeprom_probe);

  std::vector<u8> boot_rom(0x2000, 0);
  boot_rom[0]=0x80; boot_rom[1]=0x37; boot_rom[2]=0x12; boot_rom[3]=0x40;
  put_be32(boot_rom, 0x08, 0x80000400);
  const char* title="BOOTTEST"; for (int i=0;i<8;i++) boot_rom[0x20+i]=title[i];
  put_be32(boot_rom, 0x1000, 0x3C1D8040);
  put_be32(boot_rom, 0x1004, 0x27BDFFF0);
  core.rom = boot_rom; core.rom_header = N64Header(boot_rom); core.cic = CIC_NUS_6102;
  core._hle_ipl3_boot();
  CHECK(be32(core.rdram, 0x400)==0x3C1D8040);
  CHECK(be32(core.rdram, 0x404)==0x27BDFFF0);
  CHECK(core.cpu.pc==0x80000400);
  CHECK(core.bus.read_u32(0x80000400)==0x3C1D8040);
  std::vector<u8> big(0x200000, 0);
  std::memcpy(big.data(), boot_rom.data(), 0x2000);
  big[0x101000]=0xAB; big[0x101001]=0xCD; big[0x101002]=0xEF; big[0x101003]=0x01;
  core.rom = big; core._hle_ipl3_boot();
  CHECK(be32(core.rdram, 0x400 + 0x100000)==0);
  core.rom = boot_rom; core._hle_ipl3_boot();
  core.cpu.execute(N64Opcode(core.bus.read_u32(core.cpu.pc)));
  CHECK((core.cpu.gpr[29] & MASK_32)==0x80400000);

  core.running = true;
  auto t0 = std::chrono::steady_clock::now();
  int steps = core.step_frame(0.01);
  double dt = std::chrono::duration<double>(std::chrono::steady_clock::now()-t0).count();
  CHECK(steps >= INTERP_MIN_STEPS);
  CHECK(core.frame_count >= 1);
  CHECK(core.bus.hw_interrupts & MI_INTR_VI);
  CHECK(dt < FRAME_PERIOD_NTSC * 3);
  CHECK(std::fabs(core.frame_period - FRAME_PERIOD_NTSC) < 1e-6);

  core.cpu.cp0[CP0_COMPARE] = u32m(core.cpu.cp0[CP0_COUNT] + 1);
  core.cpu.cp0[CP0_CAUSE] &= ~CAUSE_IP7;
  core.bus.hw_interrupts &= ~MI_INTR_SP;
  core.cpu.step();
  CHECK(core.cpu.cp0[CP0_CAUSE] & CAUSE_IP7);
  CHECK(!(core.bus.hw_interrupts & MI_INTR_SP));

  std::vector<u8> blank(0x2000, 0);
  CHECK(identify_cic(blank).first==CIC_NUS_6102 && identify_cic(blank).second==false);
  std::vector<u8> ipl(0x2000, 0);
  for (int i=0;i<0xFC0;i++) ipl[0x40+i]=(u8)(i&0xFF); // simplified fill
  // match Python: bytes(range(256))*15 + bytes(range(0xC0))
  {
    std::vector<u8> chunk;
    for (int rep=0;rep<15;rep++) for (int b=0;b<256;b++) chunk.push_back((u8)b);
    for (int b=0;b<0xC0;b++) chunk.push_back((u8)b);
    std::memcpy(ipl.data()+0x40, chunk.data(), 0xFC0);
  }
  u32 crc = crc32_zlib(ipl.data()+0x40, 0xFC0);
  CIC_IPL3_CRC32[crc] = CIC_NUS_6105;
  auto id = identify_cic(ipl);
  CHECK(id.first==CIC_NUS_6105 && id.second==true);
  CIC_IPL3_CRC32.erase(crc);

  for (auto [cicv, seed] : {std::pair{CIC_NUS_6101,0x3F}, {CIC_NUS_6102,0x3F}, {CIC_NUS_6103,0x78},
                            {CIC_NUS_6105,0x91}, {CIC_NUS_6106,0x85}, {CIC_NUS_7102,0x3F}}) {
    for (auto [country, tv] : {std::pair{0x45,1}, {0x50,0}, {0x42,2}}) {
      boot_rom[0x3E]=(u8)country;
      core.rom=boot_rom; core.rom_header=N64Header(boot_rom); core.cic=cicv;
      core._hle_ipl3_boot();
      CHECK(core.cpu.gpr[22]==(u64)seed);
      CHECK(core.cpu.gpr[20]==(u64)tv && be32(core.rdram,0x300)==(u32)tv);
      CHECK(be32(core.rdram,0x318)==(u32)core.rdram.size() && be32(core.rdram,0x308)==0xB0000000u);
      CHECK((be32(core.rdram,0x3F0)==(u32)core.rdram.size()) == (cicv==CIC_NUS_6105));
      u32 delta = cicv==CIC_NUS_6103?0x100000:cicv==CIC_NUS_6106?0x200000:0;
      CHECK(core.cpu.pc==u32m(0x80000400-delta));
    }
  }
  boot_rom[0x3E]=0x45;
  std::vector<int> ch;
  for (int i=0;i<30;i++) ch.push_back(i&0xF);
  auto rsp = cic_6105_response(ch);
  CHECK(rsp.size()==30);
  for (int n : rsp) CHECK(n>=0 && n<=0xF);
  for (int i=0;i<0x0F;i++) core.pif_ram[0x30+i]=(u8)(0x10+i);
  core.pif_ram[0x3F]=0x02;
  core._pif_process_commands();
  CHECK(core.pif_ram[0x3F]==0 && core.pif_ram[0x2E]==0);

  std::cout << APP_NAME << ": opcode self-test passed (" << names.size() << " named ops, "
            << ULTRAHLE_PATCH_TABLE.size() << " HLE patches, "
            << steps << " steps/frame @ " << (int)(1.0/std::max(dt,1e-6)) << " target-Hz capable, dispatch complete)\n";
  return 0;
}


#if defined(CATHLE_SDL2)
// cathle 1.x Legacy UI — menus from Project64-Legacy Main.c FixMenuLang (rebranded)
static constexpr int CATHLE_GUI_W = 600, CATHLE_GUI_H = 400, CATHLE_MENU_H = 22, CATHLE_STATUS_H = 20, CATHLE_ROW = 18;

static void cathle_px(SDL_Renderer* ren, int x, int y, Uint8 R, Uint8 G, Uint8 B) {
  SDL_SetRenderDrawColor(ren, R, G, B, 255);
  SDL_RenderDrawPoint(ren, x, y);
}
static const uint8_t* cathle_glyph(char c) {
  static uint8_t G[128][7]; static bool init = false;
  if (!init) {
    init = true;
    auto set = [&](char ch, std::initializer_list<uint8_t> rows) {
      int i = 0; for (uint8_t v : rows) if (i < 7) G[(unsigned char)ch][i++] = v;
    };
    set(' ',{0,0,0,0,0,0,0}); set('.',{0,0,0,0,0,0,4}); set('-',{0,0,0,14,0,0,0});
    set(':',{0,0,4,0,4,0,0}); set('/',{2,2,4,4,8,8,0}); set('_',{0,0,0,0,0,0,31});
    set('0',{14,17,19,21,25,17,14}); set('1',{4,12,4,4,4,4,14}); set('2',{14,17,1,6,8,16,31});
    set('3',{31,2,4,2,1,17,14}); set('4',{2,6,10,18,31,2,2}); set('5',{31,16,30,1,1,17,14});
    set('6',{6,8,16,30,17,17,14}); set('7',{31,1,2,4,8,8,8}); set('8',{14,17,17,14,17,17,14});
    set('9',{14,17,17,15,1,2,12});
    set('A',{14,17,17,31,17,17,17}); set('B',{30,17,17,30,17,17,30}); set('C',{14,17,16,16,16,17,14});
    set('D',{30,17,17,17,17,17,30}); set('E',{31,16,16,30,16,16,31}); set('F',{31,16,16,30,16,16,16});
    set('G',{14,17,16,23,17,17,15}); set('H',{17,17,17,31,17,17,17}); set('I',{14,4,4,4,4,4,14});
    set('J',{1,1,1,1,17,17,14}); set('K',{17,18,20,24,20,18,17}); set('L',{16,16,16,16,16,16,31});
    set('M',{17,27,21,21,17,17,17}); set('N',{17,25,21,19,17,17,17}); set('O',{14,17,17,17,17,17,14});
    set('P',{30,17,17,30,16,16,16}); set('Q',{14,17,17,17,21,18,13}); set('R',{30,17,17,30,20,18,17});
    set('S',{15,16,16,14,1,1,30}); set('T',{31,4,4,4,4,4,4}); set('U',{17,17,17,17,17,17,14});
    set('V',{17,17,17,17,17,10,4}); set('W',{17,17,17,21,21,27,17}); set('X',{17,17,10,4,10,17,17});
    set('Y',{17,17,10,4,4,4,4}); set('Z',{31,1,2,4,8,16,31}); set('?',{14,17,1,2,4,0,4});
    set('*',{0,10,4,31,4,10,0}); set('!',{4,4,4,4,4,0,4});
    for (char ch='a'; ch<='z'; ++ch)
      for (int r=0;r<7;r++) G[(unsigned char)ch][r] = G[(unsigned char)(ch-32)][r];
  }
  return G[(unsigned char)c];
}
static void cathle_text(SDL_Renderer* ren, int x, int y, const std::string& s, Uint8 R=0, Uint8 G=0, Uint8 B=0) {
  int cx = x;
  for (char ch : s) {
    const uint8_t* rows = cathle_glyph(ch);
    for (int row=0; row<7; row++)
      for (int col=0; col<5; col++)
        if (rows[row] & (0x10 >> col)) cathle_px(ren, cx+col, y+row, R, G, B);
    cx += 6;
  }
}
static void cathle_fill(SDL_Renderer* ren, int x, int y, int w, int h, Uint8 R, Uint8 G, Uint8 B) {
  SDL_SetRenderDrawColor(ren, R, G, B, 255);
  SDL_Rect rc{x,y,w,h}; SDL_RenderFillRect(ren, &rc);
}

struct PjRom { std::string path, name, country, size, comments; };

static const char* cathle_country(u8 c) {
  switch (c) {
    case 0x45: return "USA"; case 0x4A: return "Japan"; case 0x50: return "Europe";
    case 0x44: return "Germany"; case 0x46: return "France"; case 0x49: return "Italy";
    case 0x53: return "Spain"; case 0x55: return "Australia"; case 0x58: case 0x59: return "Europe";
    default: return "Unknown";
  }
}
static std::string cathle_sz(size_t n) {
  int m = (int)std::max(1.0, std::round((n * 8.0) / (1024.0 * 1024.0)));
  return std::to_string(m) + "Mbit";
}
static bool cathle_is_rom(const std::string& low) {
  return low.size() >= 4 && (low.ends_with(".z64") || low.ends_with(".n64") || low.ends_with(".v64") || low.ends_with(".rom"));
}
static void cathle_scan(const std::string& dir, std::vector<PjRom>& out) {
  out.clear();
#if defined(_WIN32)
  WIN32_FIND_DATAA fd; HANDLE h = FindFirstFileA((dir + "\\*").c_str(), &fd);
  if (h == INVALID_HANDLE_VALUE) return;
  do {
    if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
    std::string name = fd.cFileName, low = name;
    for (char& c : low) c = (char)tolower((unsigned char)c);
    if (!cathle_is_rom(low)) continue;
    std::string path = dir + "\\" + name;
    LARGE_INTEGER li; li.LowPart = fd.nFileSizeLow; li.HighPart = fd.nFileSizeHigh;
    size_t fsz = (size_t)li.QuadPart;
#else
  DIR* d = opendir(dir.c_str()); if (!d) return;
  while (auto* ent = readdir(d)) {
    if (ent->d_name[0] == '.') continue;
    std::string name = ent->d_name, low = name;
    for (char& c : low) c = (char)tolower((unsigned char)c);
    if (!cathle_is_rom(low)) continue;
    std::string path = dir + "/" + name;
    struct stat st{}; size_t fsz = (stat(path.c_str(), &st) == 0) ? (size_t)st.st_size : 0;
#endif
    try {
      auto raw = normalize_rom_bytes(read_rom_file(path, 0x1000));
      if (raw.size() < 0x40) continue;
      N64Header hdr(raw);
      PjRom e;
      e.path = path; e.name = hdr.title.empty() ? name : hdr.title;
      e.country = cathle_country(raw.size() > 0x3E ? raw[0x3E] : 0);
      e.size = cathle_sz(fsz);
      e.comments = "?Unknown? - Update or Edit your Ini File!";
      out.push_back(std::move(e));
    } catch (...) {}
#if defined(_WIN32)
  } while (FindNextFileA(h, &fd));
  FindClose(h);
#else
  }
  closedir(d);
#endif
  std::sort(out.begin(), out.end(), [](const PjRom& a, const PjRom& b){ return a.name < b.name; });
}

static std::string cathle_open_dialog() {
#if defined(__APPLE__)
  FILE* p = popen("osascript -e 'POSIX path of (choose file with prompt \"Open Rom...\" of type {\"z64\",\"n64\",\"v64\",\"rom\",\"public.data\"})' 2>/dev/null", "r");
#elif defined(_WIN32)
  char file[MAX_PATH] = "";
  OPENFILENAMEA ofn{}; ofn.lStructSize = sizeof(ofn); ofn.lpstrFile = file; ofn.nMaxFile = MAX_PATH;
  ofn.lpstrFilter = "N64 ROM\0*.z64;*.n64;*.v64;*.rom\0All\0*.*\0";
  ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST;
  return GetOpenFileNameA(&ofn) ? std::string(file) : std::string();
#else
  FILE* p = popen("zenity --file-selection --title='Open Rom...' --file-filter='N64 ROM | *.z64 *.n64 *.v64 *.rom' 2>/dev/null", "r");
#endif
#if !defined(_WIN32)
  if (!p) return {};
  char buf[1024]; std::string out;
  while (fgets(buf, sizeof(buf), p)) out += buf;
  pclose(p);
  while (!out.empty() && (out.back()=='\n' || out.back()=='\r')) out.pop_back();
  return out;
#endif
}

static int run_cathle_legacy_gui(const std::string& start_rom) {
  if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_TIMER) != 0) {
    std::cerr << "SDL_Init: " << SDL_GetError() << "\n"; return 1;
  }
  SDL_Window* win = SDL_CreateWindow(APP_TITLE, SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
                                     CATHLE_GUI_W, CATHLE_GUI_H, 0);
  SDL_Renderer* ren = SDL_CreateRenderer(win, -1, SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
  if (!win || !ren) { std::cerr << "SDL window failed\n"; return 1; }

  ACsN64Core core;
  std::vector<PjRom> roms;
  std::string rom_dir = ".";
  if (const char* home = std::getenv("HOME")) rom_dir = std::string(home) + "/ROMs";
  cathle_scan(rom_dir, roms);
  if (roms.empty()) cathle_scan(".", roms);

  bool quit = false, browser = true, limit_fps = true, paused = false;
  int menu = -1, sel = 0, scroll = 0;
  std::string status = std::string("Ready — ") + APP_LEGACY, loaded;
  SDL_Texture* tex = nullptr; int tw = 0, th = 0;
  Uint64 fps_t0 = SDL_GetPerformanceCounter();
  int fps_frames = 0; double fps_show = 0;

  auto set_tex = [&](int w, int h) {
    if (tex && tw == w && th == h) return;
    if (tex) SDL_DestroyTexture(tex);
    tex = SDL_CreateTexture(ren, SDL_PIXELFORMAT_RGBA32, SDL_TEXTUREACCESS_STREAMING, w, h);
    tw = w; th = h;
  };
  auto load = [&](const std::string& path) {
    if (path.empty()) return;
    auto err = core.load_rom(path);
    if (!err.empty()) { status = "Error: " + err; return; }
    loaded = path; core.running = true; paused = false; browser = false;
    status = "Running — " + core.rom_header.title;
    SDL_SetWindowTitle(win, (core.rom_header.title + " - " + APP_TITLE).c_str());
  };
  if (!start_rom.empty()) load(start_rom);

  // FixMenuLang order from Project64-Legacy Main.c
  const char* top[] = {"File", "System", "Options", "Help"};
  const char* file_m[] = {
    "Open Rom...                    Ctrl+O","Rom Information                Ctrl+I","Game Information               Ctrl+G","-",
    "Start Emulation                    F10","End Emulation                      F11","-",
    "Choose Rom Directory...","Refresh Rom List                    F5","-","Exit"
  };
  const char* sys_m[] = {
    "Reset                               F1","Pause                               F2","Generate Bitmap                     F3","-",
    "Limit FPS                           F4","-","Save                                F5","Save As...","Restore                             F7",
    "Load...","-","Cheats...","GS Button                           F9"
  };
  const char* opt_m[] = {
    "Full Screen                   Alt+Enter","Always On Top","-","Configure Graphics Plugin...","Configure Audio Plugin...",
    "Configure Controller Plugin...","Configure RSP Plugin...","-","Show CPU Usage","Settings..."
  };
  const char* help_m[] = {
    "User Manual...","Game FAQ...","-","GitHub...","Homepage...","-","About Settings Files...","About cathle 1.x..."
  };
  auto items = [&](int m) -> std::pair<const char* const*, int> {
    if (m==0) return {file_m, (int)(sizeof(file_m)/sizeof(*file_m))};
    if (m==1) return {sys_m, (int)(sizeof(sys_m)/sizeof(*sys_m))};
    if (m==2) return {opt_m, (int)(sizeof(opt_m)/sizeof(*opt_m))};
    return {help_m, (int)(sizeof(help_m)/sizeof(*help_m))};
  };
  auto do_menu = [&](int m, int i) {
    if (m==0) {
      if (i==0) load(cathle_open_dialog());
      else if (i==1||i==2) { if (!loaded.empty()) { char b[64]; std::snprintf(b,sizeof(b),"%08X-%08X",core.rom_header.crc1,core.rom_header.crc2); status = core.rom_header.title + "  CRC " + b; } }
      else if (i==4) { if (!loaded.empty()) { core.running=true; paused=false; browser=false; status="Running @ 60 VI/s"; } }
      else if (i==5) { core.running=false; browser=true; status="Emulation ended"; }
      else if (i==7) status = "Rom directory: " + rom_dir;
      else if (i==8) { cathle_scan(rom_dir, roms); if (roms.empty()) cathle_scan(".", roms); status="Rom list refreshed"; }
      else if (i==10) quit = true;
    } else if (m==1) {
      if (i==0 && !loaded.empty()) { core.load_rom(loaded); core.running=true; status="Reset"; }
      else if (i==1) { paused=!paused; status = paused ? "Paused" : "Running"; }
      else if (i==4) { limit_fps=!limit_fps; status = limit_fps ? "Limit FPS: On (N64 60Hz)" : "Limit FPS: Off"; }
      else status = "Legacy stub (save/cheats) — UltraHLE core active";
    } else if (m==2) {
      if (i==0) {
        Uint32 f = SDL_GetWindowFlags(win);
        SDL_SetWindowFullscreen(win, (f & SDL_WINDOW_FULLSCREEN_DESKTOP) ? 0 : SDL_WINDOW_FULLSCREEN_DESKTOP);
      } else if (i==9) status = "Settings: UltraHLE · interpreter · 60 VI/s";
      else status = "Built-in cathle plugins (VI/RSP/RDP HLE)";
    } else {
      if (i==7) status = APP_ABOUT;
      else if (i==3) status = "cathle 1.x · legacy UI from pj64team/Project64-Legacy";
      else status = help_m[i];
    }
    menu = -1;
  };

  while (!quit) {
    Uint64 frame_start = SDL_GetPerformanceCounter();
    SDL_Event e;
    while (SDL_PollEvent(&e)) {
      if (e.type == SDL_QUIT) quit = true;
      else if (e.type == SDL_KEYDOWN) {
        auto k = e.key.keysym.sym; auto mod = SDL_GetModState();
        if (k==SDLK_ESCAPE) { if (menu>=0) menu=-1; else if (!browser) browser=true; }
        else if (k==SDLK_o && (mod&KMOD_CTRL)) load(cathle_open_dialog());
        else if (k==SDLK_F5) { cathle_scan(rom_dir, roms); status="Rom list refreshed"; }
        else if (k==SDLK_F10 && !loaded.empty()) { core.running=true; paused=false; browser=false; }
        else if (k==SDLK_F11) { core.running=false; browser=true; }
        else if (k==SDLK_F1 && !loaded.empty()) { core.load_rom(loaded); core.running=true; }
        else if (k==SDLK_F2) paused=!paused;
        else if (k==SDLK_F4) limit_fps=!limit_fps;
        else if (k==SDLK_UP && browser) sel = std::max(0, sel-1);
        else if (k==SDLK_DOWN && browser) sel = std::min(std::max(0,(int)roms.size()-1), sel+1);
        else if ((k==SDLK_RETURN||k==SDLK_KP_ENTER) && browser && !roms.empty()) load(roms[(size_t)sel].path);
      } else if (e.type == SDL_MOUSEBUTTONDOWN && e.button.button == SDL_BUTTON_LEFT) {
        int mx=e.button.x, my=e.button.y;
        if (my < CATHLE_MENU_H) {
          int slot = mx / 70; if (slot>=0 && slot<4) menu = (menu==slot) ? -1 : slot;
        } else if (menu >= 0) {
          auto mi = items(menu); int dd_x = menu*70, dd_y = CATHLE_MENU_H, dd_w = 300;
          int idx = (my - dd_y - 2) / CATHLE_ROW;
          if (mx>=dd_x && mx<=dd_x+dd_w && idx>=0 && idx<mi.second) {
            if (mi.first[idx][0] != '-') do_menu(menu, idx); else menu=-1;
          } else menu=-1;
        } else if (browser) {
          int list_y = CATHLE_MENU_H + 20;
          int row = (my - list_y) / CATHLE_ROW + scroll;
          if (row>=0 && row<(int)roms.size()) {
            sel = row;
            if (e.button.clicks >= 2) load(roms[(size_t)sel].path);
          }
        }
      } else if (e.type == SDL_MOUSEWHEEL && browser) scroll = std::max(0, scroll - e.wheel.y);
    }

    // N64 speed: one VI frame per ~1/60s when Limit FPS is on
    if (core.running && !paused && !browser) {
      double budget = limit_fps ? FRAME_PERIOD_NTSC : 0.05;
      if (core.boot_turbo && !core.fb_lit) {
        core.interp_steps = std::max(core.interp_steps, INTERP_BOOT_STEPS);
        budget = std::max(budget, 0.05);
      }
      core.step_frame(budget);
      // Only upload a lit / RDP-drawn frame — never show boot RDRAM static
      if (core.fb_lit && !core.fb_rgba.empty()) {
        set_tex(core.fb_w, core.fb_h);
        SDL_UpdateTexture(tex, nullptr, core.fb_rgba.data(), core.fb_w * 4);
      }
      fps_frames++;
      Uint64 now = SDL_GetPerformanceCounter();
      double dt = (double)(now - fps_t0) / (double)SDL_GetPerformanceFrequency();
      if (dt >= 0.5) { fps_show = fps_frames / dt; fps_frames = 0; fps_t0 = now; }
    }

    cathle_fill(ren, 0, 0, CATHLE_GUI_W, CATHLE_GUI_H, 0xd4, 0xd0, 0xc8);
    cathle_fill(ren, 0, 0, CATHLE_GUI_W, CATHLE_MENU_H, 0xd4, 0xd0, 0xc8);
    SDL_SetRenderDrawColor(ren, 0x80,0x80,0x80,255);
    SDL_RenderDrawLine(ren, 0, CATHLE_MENU_H-1, CATHLE_GUI_W, CATHLE_MENU_H-1);
    for (int i=0;i<4;i++) {
      if (menu==i) cathle_fill(ren, i*70, 0, 70, CATHLE_MENU_H, 0x00,0x00,0x80);
      cathle_text(ren, i*70+6, 7, top[i], menu==i?255:0, menu==i?255:0, menu==i?255:0);
    }

    int cy = CATHLE_MENU_H, ch = CATHLE_GUI_H - CATHLE_MENU_H - CATHLE_STATUS_H;
    if (browser) {
      cathle_text(ren, 6, cy+6, "Good Name");
      cathle_text(ren, 260, cy+6, "Country");
      cathle_text(ren, 330, cy+6, "Size");
      cathle_text(ren, 400, cy+6, "Comments");
      SDL_SetRenderDrawColor(ren,0x80,0x80,0x80,255);
      SDL_RenderDrawLine(ren, 0, cy+19, CATHLE_GUI_W, cy+19);
      int list_y = cy + 20, visible = (ch - 20) / CATHLE_ROW;
      if (sel < scroll) scroll = sel;
      if (sel >= scroll + visible) scroll = sel - visible + 1;
      for (int i=0;i<visible;i++) {
        int idx = scroll + i; if (idx >= (int)roms.size()) break;
        int y = list_y + i * CATHLE_ROW;
        if (idx == sel) cathle_fill(ren, 0, y, CATHLE_GUI_W, CATHLE_ROW, 0x00,0x00,0x80);
        Uint8 t = (idx==sel) ? 255 : 0;
        auto clip=[&](std::string s, int n){ if ((int)s.size()>n) s=s.substr(0,(size_t)n-2)+".."; return s; };
        auto& r = roms[(size_t)idx];
        cathle_text(ren, 6, y+5, clip(r.name,40), t,t,t);
        cathle_text(ren, 260, y+5, clip(r.country,10), t,t,t);
        cathle_text(ren, 330, y+5, clip(r.size,10), t,t,t);
        cathle_text(ren, 400, y+5, clip(r.comments,30), t,t,t);
      }
      if (roms.empty())
        cathle_text(ren, 30, list_y+40, "No ROMs — File > Open Rom... or put .z64 in ~/ROMs");
    } else {
      cathle_fill(ren, 0, cy, CATHLE_GUI_W, ch, 0,0,0);
      if (tex && core.fb_lit) {
        SDL_Rect dst{0, cy, CATHLE_GUI_W, ch};
        SDL_RenderCopy(ren, tex, nullptr, &dst);
      } else {
        cathle_text(ren, 200, cy+ch/2 - 8, APP_TITLE, 180,180,180);
        cathle_text(ren, 140, cy+ch/2 + 8, "booting — waiting for first lit frame", 140,140,140);
      }
    }

    if (menu >= 0) {
      auto mi = items(menu);
      int dd_x = menu*70, dd_y = CATHLE_MENU_H, dd_w = 310, dd_h = mi.second * CATHLE_ROW + 4;
      cathle_fill(ren, dd_x, dd_y, dd_w, dd_h, 0xd4,0xd0,0xc8);
      SDL_SetRenderDrawColor(ren,0x40,0x40,0x40,255);
      SDL_Rect br{dd_x,dd_y,dd_w,dd_h}; SDL_RenderDrawRect(ren, &br);
      for (int i=0;i<mi.second;i++) {
        if (mi.first[i][0]=='-') {
          SDL_SetRenderDrawColor(ren,0x80,0x80,0x80,255);
          int yy = dd_y+2+i*CATHLE_ROW+CATHLE_ROW/2;
          SDL_RenderDrawLine(ren, dd_x+4, yy, dd_x+dd_w-4, yy);
        } else cathle_text(ren, dd_x+8, dd_y+2+i*CATHLE_ROW+5, mi.first[i]);
      }
    }

    int sy = CATHLE_GUI_H - CATHLE_STATUS_H;
    cathle_fill(ren, 0, sy, CATHLE_GUI_W, CATHLE_STATUS_H, 0xd4,0xd0,0xc8);
    SDL_SetRenderDrawColor(ren,0x80,0x80,0x80,255);
    SDL_RenderDrawLine(ren, 0, sy, CATHLE_GUI_W, sy);
    // Project64 Legacy-style status panes: message | CORE | FPS
    cathle_text(ren, 6, sy+6, status.substr(0, 48));
    cathle_text(ren, 320, sy+6, core.running ? (paused ? "PAUSE" : "r4300i") : "IDLE");
    char fpsb[48];
    if (core.running && !paused && !browser)
      std::snprintf(fpsb, sizeof(fpsb), "FPS: %.1f", fps_show);
    else
      std::snprintf(fpsb, sizeof(fpsb), "FPS: -.--");
    cathle_text(ren, CATHLE_GUI_W - 90, sy+6, fpsb);

    SDL_RenderPresent(ren);

    if (limit_fps && core.running && !paused && !browser) {
      double target = FRAME_PERIOD_NTSC;
      double elapsed = (double)(SDL_GetPerformanceCounter() - frame_start) / (double)SDL_GetPerformanceFrequency();
      if (elapsed < target) SDL_Delay((Uint32)((target - elapsed) * 1000.0));
    } else {
      SDL_Delay(8);
    }
  }

  if (tex) SDL_DestroyTexture(tex);
  SDL_DestroyRenderer(ren); SDL_DestroyWindow(win); SDL_Quit();
  return 0;
}
#endif // CATHLE_SDL2

int main(int argc, char** argv) {
  init_dispatch();
  init_ultrahle_tables();
  std::vector<std::string> args;
  for (int i=1;i<argc;i++) args.push_back(argv[i]);
  bool self=false, opcodes=false, nogui=false;
  std::string rom_arg;
  for (auto& a : args) {
    if (a=="--self-test") self=true;
    else if (a=="--opcodes") opcodes=true;
    else if (a=="--nogui") nogui=true;
    else if (!a.empty() && a[0]!='-') rom_arg=a;
  }
  if (opcodes) {
    assert_opcode_table();
    for (auto& n : list_implemented_opcodes()) std::cout << n << "\n";
    return 0;
  }
  if (self) {
    try { return self_test(); }
    catch (const std::exception& e) { std::cerr << "self-test exception: " << e.what() << "\n"; return 1; }
  }
#if defined(CATHLE_SDL2)
  if (!nogui) return run_cathle_legacy_gui(rom_arg);
#endif
  if (!rom_arg.empty()) {
    ACsN64Core core;
    auto err = core.load_rom(rom_arg);
    if (!err.empty()) { std::cerr << err << "\n"; return 1; }
    core.running = true;
    for (int i=0;i<120;i++) core.step_frame(FRAME_PERIOD_NTSC);
    std::cout << APP_NAME << ": 120 frames @ N64 60Hz, lit=" << (core.fb_lit?1:0) << "\n";
    return 0;
  }
  if (args.empty()) {
    try { return self_test(); }
    catch (const std::exception& e) { std::cerr << "self-test exception: " << e.what() << "\n"; return 1; }
  }
  std::cout << "Usage: " << argv[0] << " [--self-test|--opcodes|--nogui] [rom.z64]\n";
  return 0;
}
