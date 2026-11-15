#include "libs/cpp_hash_lib_wrapper.h"

#include <iostream>
#include <unordered_map>
#include <vector>

extern "C" {
#include "globals/global_defs.h"
}

// Synthetic "patch" instructions (ctx_switch_barrier_, etc.) reuse a real instruction's
// address but carry their own fabricated (always-zeroed) binary encoding, so {addr,
// lsb_bytes, msb_bytes, op_idx, mem_shape} alone isn't enough to tell them apart from
// whatever real instruction happens to hash to the same key. cf_type is included in every
// one of these tables (dynamic Inst_Info as well as the static per-macro/per-uop tables)
// so a patch can never alias a real decode, or vice versa.
struct inst_key {
  uint64_t addr;
  uint64_t lsb_bytes;
  uint64_t msb_bytes;
  uint8_t op_idx;
  uint16_t mem_shape;
  uint8_t cf_type;

  inst_key() : addr(0), lsb_bytes(0), msb_bytes(0), op_idx(0), mem_shape(0), cf_type(0) {}
  inst_key(uint64_t _addr, uint64_t _lsb_bytes, uint64_t _msb_bytes, uint8_t _op_idx, uint16_t _mem_shape,
           uint8_t _cf_type)
      : addr(_addr),
        lsb_bytes(_lsb_bytes),
        msb_bytes(_msb_bytes),
        op_idx(_op_idx),
        mem_shape(_mem_shape),
        cf_type(_cf_type) {}

  bool operator==(const inst_key& p) const {
    return addr == p.addr && lsb_bytes == p.lsb_bytes && msb_bytes == p.msb_bytes && op_idx == p.op_idx &&
           mem_shape == p.mem_shape && cf_type == p.cf_type;
  }
};

struct inst_hash_fn {
  std::size_t operator()(const inst_key& key) const {
    const uint64_t KNUTH64 = 11400714819323198485ull;

    uint64_t h = key.addr;
    h *= KNUTH64;
    h += key.lsb_bytes;
    h *= KNUTH64;
    h += key.msb_bytes;
    h *= KNUTH64;
    h += static_cast<uint64_t>(key.op_idx);
    h *= KNUTH64;
    h += static_cast<uint64_t>(key.mem_shape);
    h *= KNUTH64;
    h += static_cast<uint64_t>(key.cf_type);

    return static_cast<std::size_t>(h ^ (h >> 32));
  }
};

// Per-core hash maps for instruction info
std::unordered_map<inst_key, Inst_Info, inst_hash_fn> per_core_hash_map[MAX_NUM_PROCS];

Inst_Info* cpp_hash_table_access_create(int core, uint64_t addr, uint64_t lsb_bytes, uint64_t msb_bytes, uint8_t op_idx,
                                        uint16_t mem_shape, uint8_t cf_type, unsigned char* new_entry) {
  *new_entry = false;
  inst_key _key(addr, lsb_bytes, msb_bytes, op_idx, mem_shape, cf_type);
  auto& hash_map = per_core_hash_map[core];
  auto lookup = hash_map.find(_key);
  if (lookup != hash_map.end()) {
    return &lookup->second;
  } else {
    auto inserted = hash_map.emplace(_key, Inst_Info{});
    *new_entry = inserted.second;
    return &inserted.first->second;
  }
}

// Per-macro-instruction static info: keyed by {addr, binary, mem_shape, cf_type} (op_idx fixed
// at 0), so all uops of the same macro instance share one entry.
std::unordered_map<inst_key, Static_Inst_Info, inst_hash_fn> per_core_static_inst_map[MAX_NUM_PROCS];

// Direct-mapped, PC-indexed front cache for per_core_static_inst_map. Decode looks the per-macro
// Static_Inst_Info up once per macro, and a map lookup is a 64-bit div plus a chain of dependent
// loads into ~250 B nodes. A hit is a full-key match on an entry the map already holds, so it
// returns exactly the pointer (and *new_entry = false) the map lookup would: the map is never
// erased and its nodes never move. A miss falls through to the map and refills the slot.
namespace {
struct Static_Inst_Cache_Entry {
  uint64_t addr;
  uint64_t lsb_bytes;
  uint64_t msb_bytes;
  Static_Inst_Info* si;  // nullptr: empty slot
  uint16_t mem_shape;
  uint8_t cf_type;
};
constexpr unsigned STATIC_INST_CACHE_LOG2 = 16;  // entries per core, allocated on first use
std::vector<Static_Inst_Cache_Entry> static_inst_cache[MAX_NUM_PROCS];

inline size_t static_inst_cache_idx(uint64_t addr) {
  return (addr ^ (addr >> STATIC_INST_CACHE_LOG2)) & ((size_t{1} << STATIC_INST_CACHE_LOG2) - 1);
}
}  // namespace

Static_Inst_Info* cpp_static_inst_access_create(int core, uint64_t addr, uint64_t lsb_bytes, uint64_t msb_bytes,
                                                uint16_t mem_shape, uint8_t cf_type, unsigned char* new_entry) {
  *new_entry = false;
  auto& cache = static_inst_cache[core];
  if (cache.empty())
    cache.resize(size_t{1} << STATIC_INST_CACHE_LOG2);  // value-initialized: every si is nullptr
  Static_Inst_Cache_Entry& slot = cache[static_inst_cache_idx(addr)];
  if (slot.si && slot.addr == addr && slot.lsb_bytes == lsb_bytes && slot.msb_bytes == msb_bytes &&
      slot.mem_shape == mem_shape && slot.cf_type == cf_type)
    return slot.si;

  inst_key _key(addr, lsb_bytes, msb_bytes, 0, mem_shape, cf_type);
  auto& hash_map = per_core_static_inst_map[core];
  auto lookup = hash_map.find(_key);
  Static_Inst_Info* si;
  if (lookup != hash_map.end()) {
    si = &lookup->second;
  } else {
    auto inserted = hash_map.emplace(_key, Static_Inst_Info{});
    *new_entry = inserted.second;
    si = &inserted.first->second;
  }
  slot = {addr, lsb_bytes, msb_bytes, si, mem_shape, cf_type};
  return si;
}

// Per-uop static info: keyed by {addr, binary, op_idx, mem_shape, cf_type}.
std::unordered_map<inst_key, Static_Op_Info, inst_hash_fn> per_core_static_op_map[MAX_NUM_PROCS];

Static_Op_Info* cpp_static_op_access_create(int core, uint64_t addr, uint64_t lsb_bytes, uint64_t msb_bytes,
                                            uint8_t op_idx, uint16_t mem_shape, uint8_t cf_type,
                                            unsigned char* new_entry) {
  *new_entry = false;
  inst_key _key(addr, lsb_bytes, msb_bytes, op_idx, mem_shape, cf_type);
  auto& hash_map = per_core_static_op_map[core];
  auto lookup = hash_map.find(_key);
  if (lookup != hash_map.end()) {
    return &lookup->second;
  } else {
    auto inserted = hash_map.emplace(_key, Static_Op_Info{});
    *new_entry = inserted.second;
    return &inserted.first->second;
  }
}
