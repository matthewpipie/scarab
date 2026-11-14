/* Copyright 2026 HPS/SAFARI Research Groups
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 */

/***************************************************************************************
 * File         : prefetcher/pdip.cc
 * Description  : Priority Directed Instruction Prefetching (PDIP).
 *                Godala et al., "PDIP: Priority Directed Instruction Prefetching", ASPLOS'24.
 *
 * FDIP already hides icache misses whose line enters the FTQ early enough. PDIP only targets
 * the ones it doesn't: front-end-critical (FEC) lines, whose miss starved the front end.
 *
 * Detection (paper §2.1, §3, §5.3). The icache stage tracks each demand fetch that misses
 * both the uop cache and the icache. Every cycle fetch delivers nothing while waiting on it is
 * a starvation cycle; if the issue queue was empty on any of them, the back end stalled too.
 * When the line arrives, a miss with more than --pdip_fec_cycles starvation cycles and an
 * empty-issue-queue cycle (a "high cost FEC line with back-end stall") stamps the first op
 * served from it. Training happens when that op retires, so a line fetched only on the wrong
 * path never trains. Only signals a real core has drive this; ic->off_path is not consulted.
 *
 * Trigger (§4.2). A miss that begins while the FTQ has not been full since the last resteer
 * is blamed on the resteering branch (an exec-time mispredict or a decode-time BTB miss /
 * misfetch; L0 early corrections are not resteers here). Otherwise the FTQ had its full lead
 * time and the miss was too long to hide: it is blamed on the last taken branch retired when
 * the miss began. Return triggers are not inserted (§5.2), and each retired FEC op is inserted
 * with probability 1/--pdip_insert_one_in (§5.3) from a per-core xorshift generator.
 *
 * Table (§5.1, §5.4). --pdip_sets x --pdip_ways, indexed by the trigger's cache-line ("block")
 * address with a --pdip_tag_bits tag and one NRU bit per way. Each entry holds --pdip_targets
 * target lines, newest first, each with a --pdip_mask_bits mask of the following lines that
 * also missed (bit i: line + i + 1). Targets are stored as full line addresses.
 *
 * Lookup and prefetch (§5). Scarab's fetch targets end at a taken branch or an icache-line
 * boundary, so each one entering the FTQ is one block: its line is looked up once. On a hit
 * the entry's targets (and their masked neighbours) go to a --pdip_pq_size prefetch queue
 * when it has room and the icache MSHRs have more than --pdip_mshr_reserve free entries.
 * update_pdip moves up to --pdip_issue_width entries a cycle to the icache's prefetch queue,
 * behind that cycle's FDIP and EIP lines: each is dropped if the MSHRs are short or
 * (--pdip_ftq_dedup) its line is already in the FTQ. A full icache queue leaves the entry for
 * the next cycle. Like FDIP's and EIP's, a queued line's tag lookup takes a bank port of the
 * icache ICACHE_LATENCY - 1 cycles later (pref_drain_queue); only a miss goes to the L2, as
 * an MRT_IPRF (pdip_pref_issued).
 ***************************************************************************************/

#include "prefetcher/pdip.h"

#include "globals/assert.h"
#include "globals/global_defs.h"
#include "globals/global_types.h"
#include "globals/global_vars.h"
#include "globals/utils.h"

#include "debug/debug_macros.h"
#include "debug/debug_print.h"

#include "decoupled_frontend.h"
#include "statistics.h"

extern "C" {
#include "debug/debug.param.h"

#include "general.param.h"
#include "memory/memory.param.h"
#include "prefetcher/pref.param.h"

#include "bp/bp.h"
#include "memory/memory.h"
#include "memory/mshr.h"

#include "icache_stage.h"
#include "op.h"
}

#include <algorithm>
#include <deque>
#include <vector>

#define DEBUG(proc_id, args...) _DEBUG(proc_id, DEBUG_PDIP, ##args)

namespace {

struct Pdip_Target {
  Addr line;  // 0: empty slot
  uns mask;   // bit i: also prefetch line + (i + 1) lines
};

struct Pdip_Entry {
  Flag valid;
  uns tag;
  Flag nru;                          // the one "LRU" bit per way of §5.4
  std::vector<Pdip_Target> targets;  // newest first
};

struct Pdip_Trigger {
  Addr line;  // 0: none
  Cf_Type cf;
};

struct Pdip_Miss {
  Flag active;
  Addr line;
  uns stall_cycles;
  Flag iq_empty;
  Pdip_Trigger_Kind kind;
  Pdip_Trigger trigger;
};

struct Pdip_Core {
  std::vector<Pdip_Entry> table;  // set-major: sets * ways
  uns set_bits = 0;

  Flag resteer_seen = FALSE;              // a resteer has happened since the start
  Pdip_Trigger resteer = {0, NOT_CF};     // the last resteer's branch (line 0: it wasn't a branch)
  Flag ftq_full_since_resteer = TRUE;     // FDIP has had its full lead time since then
  Pdip_Trigger last_taken = {0, NOT_CF};  // the last taken branch retired

  Pdip_Miss miss = {};     // the demand miss fetch is waiting on
  Pdip_Miss pending = {};  // an FEC line that has arrived, waiting for its first op (active: armed)

  uns32 rng = 0;
  std::deque<Addr> pq;
};

std::vector<Pdip_Core> pdip_cores;

inline Addr pdip_line(Addr addr) {
  return ROUND_DOWN(addr, (Addr)ICACHE_LINE_SIZE);  // an uns mask would clear bits 32-63
}

inline uns pdip_set(const Pdip_Core& c, Addr line) {
  UNUSED(c);
  return (line >> LOG2(ICACHE_LINE_SIZE)) & (PDIP_SETS - 1);
}

inline uns pdip_tag(const Pdip_Core& c, Addr line) {
  return ((line >> LOG2(ICACHE_LINE_SIZE)) >> c.set_bits) & ((1u << PDIP_TAG_BITS) - 1);
}

inline Pdip_Entry* pdip_set_base(Pdip_Core& c, Addr line) {
  return &c.table[pdip_set(c, line) * PDIP_WAYS];
}

/* The way holding `line`'s tag, or -1. */
int pdip_find(Pdip_Core& c, Addr line) {
  Pdip_Entry* base = pdip_set_base(c, line);
  uns tag = pdip_tag(c, line);
  for (uns w = 0; w < PDIP_WAYS; w++)
    if (base[w].valid && base[w].tag == tag)
      return (int)w;
  return -1;
}

/* NRU: mark the way used; once every way is marked, clear all the others. */
void pdip_touch(Pdip_Entry* base, uns way) {
  base[way].nru = TRUE;
  for (uns w = 0; w < PDIP_WAYS; w++)
    if (!base[w].nru)
      return;
  for (uns w = 0; w < PDIP_WAYS; w++)
    if (w != way)
      base[w].nru = FALSE;
}

uns pdip_victim(Pdip_Entry* base) {
  for (uns w = 0; w < PDIP_WAYS; w++)
    if (!base[w].valid)
      return w;
  for (uns w = 0; w < PDIP_WAYS; w++)
    if (!base[w].nru)
      return w;
  return 0;  // only reachable with one way
}

uns32 pdip_rand(Pdip_Core& c) {
  uns32 x = c.rng;
  x ^= x << 13;
  x ^= x >> 17;
  x ^= x << 5;
  c.rng = x;
  return x;
}

Flag pdip_mshr_room(uns proc_id) {
  Mshr* mshr = mem_core_file(proc_id, MRT_IPRF);
  return mshr->size - mshr_count(mshr) > PDIP_MSHR_RESERVE;
}

void pdip_insert(uns proc_id, Pdip_Core& c, Addr trigger, Addr target) {
  Pdip_Entry* base = pdip_set_base(c, trigger);
  int way = pdip_find(c, trigger);
  if (way < 0) {
    way = (int)pdip_victim(base);
    Pdip_Entry& e = base[way];
    if (e.valid)
      STAT_EVENT(proc_id, PDIP_TABLE_EVICT);
    e.valid = TRUE;
    e.tag = pdip_tag(c, trigger);
    for (auto& t : e.targets)
      t = Pdip_Target{0, 0};
    e.targets[0] = Pdip_Target{target, 0};
    pdip_touch(base, (uns)way);
    STAT_EVENT(proc_id, PDIP_INSERT_NEW_ENTRY);
    DEBUG(proc_id, "insert trigger:0x%llx target:0x%llx new entry set:%u way:%d\n", (unsigned long long)trigger,
          (unsigned long long)target, pdip_set(c, trigger), way);
    return;
  }

  pdip_touch(base, (uns)way);
  Pdip_Entry& e = base[way];
  for (auto& t : e.targets) {
    if (!t.line)
      continue;
    if (target == t.line) {
      STAT_EVENT(proc_id, PDIP_INSERT_SAME);
      return;
    }
    if (target > t.line) {
      Addr delta = (target - t.line) >> LOG2(ICACHE_LINE_SIZE);
      if (delta <= PDIP_MASK_BITS) {
        uns bit = 1u << (delta - 1);
        STAT_EVENT(proc_id, (t.mask & bit) ? PDIP_INSERT_SAME : PDIP_INSERT_MASK);
        t.mask |= bit;
        return;
      }
    }
  }

  // A new target: newest first, the oldest falls off the end.
  STAT_EVENT(proc_id, e.targets.back().line ? PDIP_INSERT_REPL_TARGET : PDIP_INSERT_NEW_TARGET);
  for (size_t i = e.targets.size() - 1; i > 0; i--)
    e.targets[i] = e.targets[i - 1];
  e.targets[0] = Pdip_Target{target, 0};
  DEBUG(proc_id, "insert trigger:0x%llx target:0x%llx into set:%u way:%d\n", (unsigned long long)trigger,
        (unsigned long long)target, pdip_set(c, trigger), way);
}

void pdip_pq_enqueue(uns proc_id, Pdip_Core& c, Addr line) {
  if (std::find(c.pq.begin(), c.pq.end(), line) != c.pq.end()) {
    STAT_EVENT(proc_id, PDIP_PQ_DROP_DUP);
  } else if (c.pq.size() >= PDIP_PQ_SIZE) {
    STAT_EVENT(proc_id, PDIP_PQ_DROP_FULL);
  } else if (!pdip_mshr_room(proc_id)) {
    STAT_EVENT(proc_id, PDIP_PQ_DROP_MSHR);
  } else {
    c.pq.push_back(line);
    STAT_EVENT(proc_id, PDIP_PQ_ENQ);
  }
}

}  // namespace

/**************************************************************************************/
/* Setup */

void alloc_mem_pdip(uns num_cores) {
  pdip_cores.resize(num_cores);
}

void init_pdip(uns proc_id) {
  if (!PDIP_ENABLE)
    return;
  ASSERTM(proc_id, PDIP_SETS && !(PDIP_SETS & (PDIP_SETS - 1)), "--pdip_sets must be a power of two\n");
  ASSERTM(proc_id, PDIP_WAYS >= 1 && PDIP_TARGETS >= 1, "--pdip_ways and --pdip_targets must be at least 1\n");
  ASSERTM(proc_id, PDIP_TAG_BITS >= 1 && PDIP_TAG_BITS <= 31, "--pdip_tag_bits must be in [1, 31]\n");
  ASSERTM(proc_id, PDIP_MASK_BITS <= 31, "--pdip_mask_bits must be at most 31\n");
  ASSERTM(proc_id, PDIP_INSERT_ONE_IN >= 1, "--pdip_insert_one_in must be at least 1\n");
  ASSERTM(proc_id, PDIP_TRIGGER_MODE <= 2, "--pdip_trigger_mode must be 0, 1 or 2\n");
  ASSERTM(proc_id, PDIP_PQ_SIZE >= 1 && PDIP_ISSUE_WIDTH >= 1, "--pdip_pq_size and --pdip_issue_width must be >= 1\n");

  Pdip_Core& c = pdip_cores[proc_id];
  c.set_bits = LOG2(PDIP_SETS);
  c.table.assign(PDIP_SETS * PDIP_WAYS, Pdip_Entry{FALSE, 0, FALSE, std::vector<Pdip_Target>(PDIP_TARGETS)});
  c.resteer_seen = FALSE;
  c.resteer = Pdip_Trigger{0, NOT_CF};
  c.ftq_full_since_resteer = TRUE;
  c.last_taken = Pdip_Trigger{0, NOT_CF};
  c.miss = Pdip_Miss{};
  c.pending = Pdip_Miss{};
  c.rng = 0x9E3779B9u ^ (proc_id + 1);
  c.pq.clear();
}

/**************************************************************************************/
/* Decoupled front end */

void pdip_ftq_push(uns proc_id, Addr line, Flag off_path) {
  Pdip_Core& c = pdip_cores[proc_id];
  line = pdip_line(line);
  STAT_EVENT(proc_id, PDIP_LOOKUP_ONPATH + (off_path ? 1 : 0));
  int way = pdip_find(c, line);
  if (way < 0)
    return;
  STAT_EVENT(proc_id, PDIP_LOOKUP_HIT_ONPATH + (off_path ? 1 : 0));
  Pdip_Entry* base = pdip_set_base(c, line);
  pdip_touch(base, (uns)way);
  DEBUG(proc_id, "lookup hit 0x%llx set:%u way:%d\n", (unsigned long long)line, pdip_set(c, line), way);
  if (!PDIP_PREFETCH)
    return;
  for (const auto& t : base[way].targets) {
    if (!t.line)
      continue;
    pdip_pq_enqueue(proc_id, c, t.line);
    for (uns i = 0; i < PDIP_MASK_BITS; i++)
      if (t.mask & (1u << i))
        pdip_pq_enqueue(proc_id, c, t.line + (Addr)(i + 1) * ICACHE_LINE_SIZE);
  }
}

void pdip_note_ftq_full(uns proc_id) {
  pdip_cores[proc_id].ftq_full_since_resteer = TRUE;
}

/**************************************************************************************/
/* Icache stage: FEC detection */

void pdip_demand_miss(uns proc_id, Addr line) {
  Pdip_Core& c = pdip_cores[proc_id];
  Pdip_Miss& m = c.miss;
  m = Pdip_Miss{};
  m.active = TRUE;
  m.line = pdip_line(line);
  STAT_EVENT(proc_id, PDIP_MISS_TRACKED);

  const Flag resteer_path = c.resteer_seen && !c.ftq_full_since_resteer;
  Pdip_Trigger_Kind kind = PDIP_TRIG_NONE;
  switch (PDIP_TRIGGER_MODE) {
    case 0:  // paper: resteer path -> the resteering branch, else the last retired taken branch
      kind = (resteer_path && c.resteer.line) ? PDIP_TRIG_RESTEER : PDIP_TRIG_LASTTAKEN;
      break;
    case 1:  // resteer triggers only
      kind = (resteer_path && c.resteer.line) ? PDIP_TRIG_RESTEER : PDIP_TRIG_NONE;
      break;
    case 2:  // last-taken triggers only
      kind = PDIP_TRIG_LASTTAKEN;
      break;
  }
  m.trigger = kind == PDIP_TRIG_RESTEER     ? c.resteer
              : kind == PDIP_TRIG_LASTTAKEN ? c.last_taken
                                            : Pdip_Trigger{0, NOT_CF};
  m.kind = m.trigger.line ? kind : PDIP_TRIG_NONE;
}

void pdip_miss_stall_cycle(uns proc_id, Addr line, Flag iq_empty) {
  Pdip_Miss& m = pdip_cores[proc_id].miss;
  if (!m.active || m.line != pdip_line(line))
    return;
  m.stall_cycles++;
  m.iq_empty |= iq_empty;
  STAT_EVENT(proc_id, PDIP_MISS_STALL_CYCLES);
}

void pdip_demand_fill(uns proc_id, Addr line) {
  Pdip_Core& c = pdip_cores[proc_id];
  Pdip_Miss& m = c.miss;
  if (!m.active || m.line != pdip_line(line))
    return;
  m.active = FALSE;
  if (m.stall_cycles <= PDIP_FEC_CYCLES) {
    STAT_EVENT(proc_id, PDIP_FEC_REJECT_CYCLES);
  } else if (PDIP_REQUIRE_IQ_EMPTY && !m.iq_empty) {
    STAT_EVENT(proc_id, PDIP_FEC_REJECT_IQ);
  } else if (m.kind == PDIP_TRIG_NONE) {
    STAT_EVENT(proc_id, PDIP_FEC_REJECT_NO_TRIGGER);
  } else {
    STAT_EVENT(proc_id, m.kind == PDIP_TRIG_RESTEER ? PDIP_FEC_MARKED_RESTEER : PDIP_FEC_MARKED_LASTTAKEN);
    c.pending = m;
    c.pending.active = TRUE;
    DEBUG(proc_id, "FEC line 0x%llx stall:%u kind:%d trigger:0x%llx\n", (unsigned long long)m.line, m.stall_cycles,
          (int)m.kind, (unsigned long long)m.trigger.line);
  }
}

void pdip_mark_op(uns proc_id, Op* op) {
  Pdip_Miss& p = pdip_cores[proc_id].pending;
  if (!p.active)
    return;
  p.active = FALSE;
  if (pdip_line(op->inst->addr) != p.line) {
    STAT_EVENT(proc_id, PDIP_FEC_MARK_LOST);
    return;
  }
  op->pdip_target = p.line;
  op->pdip_trigger = p.trigger.line;
  op->pdip_trigger_kind = (uns8)p.kind;
  op->pdip_trigger_cf = (uns8)p.trigger.cf;
  op->pdip_stall_cycles = p.stall_cycles;
}

/**************************************************************************************/
/* Recovery */

void pdip_recover(uns proc_id, Flag fetch_redirected) {
  Pdip_Core& c = pdip_cores[proc_id];
  if (fetch_redirected) {  // fetch leaves the line it was waiting on
    c.miss.active = FALSE;
    c.pending.active = FALSE;
  }

  Op* op = bp_recovery_info->recovery_op;
  ASSERT(proc_id, op);
  const bool is_early_recovery = bp_l0_enabled() && op->bp_pred_l0.recovery_point == RECOVER_AT_FE &&
                                 op->bp_pred_main.recovery_point != RECOVER_AT_EXEC &&
                                 op->bp_pred_main.recovery_point != RECOVER_AT_DECODE;
  if (is_early_recovery) {
    STAT_EVENT(proc_id, PDIP_RESTEER_SKIP_FE);
    return;
  }
  STAT_EVENT(proc_id, op->bp_pred_main.recovery_point == RECOVER_AT_DECODE ? PDIP_RESTEER_DECODE : PDIP_RESTEER_EXEC);
  c.resteer_seen = TRUE;
  c.ftq_full_since_resteer = FALSE;
  const Cf_Type cf = (Cf_Type)bp_recovery_info->recovery_cf_type;
  if (cf != NOT_CF && cf != CF_SYS) {
    c.resteer = Pdip_Trigger{pdip_line(bp_recovery_info->recovery_info.PC), cf};
  } else {
    c.resteer = Pdip_Trigger{0, NOT_CF};
    STAT_EVENT(proc_id, PDIP_RESTEER_NOT_BRANCH);
  }
}

/**************************************************************************************/
/* Retirement: training */

void pdip_retire(Op* op) {
  const uns proc_id = op->proc_id;
  Pdip_Core& c = pdip_cores[proc_id];

  if (op->pdip_target) {
    STAT_EVENT(proc_id,
               op->pdip_trigger_kind == PDIP_TRIG_RESTEER ? PDIP_FEC_RETIRED_RESTEER : PDIP_FEC_RETIRED_LASTTAKEN);
    INC_STAT_EVENT(proc_id, PDIP_FEC_RETIRED_STALL_CYCLES, op->pdip_stall_cycles);
    if (PDIP_SKIP_RET && op->pdip_trigger_cf == CF_RET)
      STAT_EVENT(proc_id, PDIP_INSERT_SKIP_RET);
    else if (pdip_rand(c) % PDIP_INSERT_ONE_IN)
      STAT_EVENT(proc_id, PDIP_INSERT_SKIP_PROB);
    else
      pdip_insert(proc_id, c, op->pdip_trigger, op->pdip_target);
  }

  const Cf_Type cf = op->uop->cf_type;
  if (cf != NOT_CF && cf != CF_REP && cf != CF_SYS && op->oracle_info.dir == TAKEN)
    c.last_taken = Pdip_Trigger{pdip_line(op->inst->addr), cf};
}

/**************************************************************************************/
/* Prefetch queue */

void update_pdip(uns proc_id) {
  Pdip_Core& c = pdip_cores[proc_id];
  for (uns processed = 0; !c.pq.empty() && processed < PDIP_ISSUE_WIDTH;) {
    const Addr line = c.pq.front();
    if (!pdip_mshr_room(proc_id)) {
      c.pq.pop_front();
      processed++;
      STAT_EVENT(proc_id, PDIP_ISSUE_DROP_MSHR);
      continue;
    }
    if (PDIP_FTQ_DEDUP && decoupled_fe_ftq_has_line(proc_id, line)) {
      c.pq.pop_front();
      processed++;
      STAT_EVENT(proc_id, PDIP_ISSUE_DROP_FTQ);
      continue;
    }
    if (!pref_addto_icache_queue(proc_id, line, PDIP_PREF_ID, MRT_IPRF, FALSE, FALSE, 0, 0)) {
      STAT_EVENT(proc_id, PDIP_ICQ_RETRY);
      break;
    }
    c.pq.pop_front();
    processed++;
    STAT_EVENT(proc_id, PDIP_ICQ_ENQ);
  }
}

void pdip_pref_issued(uns proc_id, Addr line, Pref_Issue_Result result) {
  switch (result) {
    case PREF_ISSUE_BANK_BUSY:
      STAT_EVENT(proc_id, PDIP_ISSUE_DROP_PORT);
      break;
    case PREF_ISSUE_HIT:
      STAT_EVENT(proc_id, PDIP_ISSUE_DROP_ICACHE_HIT);
      break;
    case PREF_ISSUE_NEW: {
      Mshr_Line* mshr_ln = mshr_line(mem_core_file(proc_id, MRT_IPRF), line);
      Mem_Req* req = mshr_ln ? mshr_line_first(mshr_ln) : NULL;
      if (req)
        req->pdip_pref = TRUE;
      STAT_EVENT(proc_id, PDIP_PREF_ISSUED);
      DEBUG(proc_id, "prefetch 0x%llx\n", (unsigned long long)line);
      break;
    }
    case PREF_ISSUE_MERGED:
      STAT_EVENT(proc_id, PDIP_PREF_MERGED);
      break;
    default:
      STAT_EVENT(proc_id, PDIP_PREF_FAILED);
  }
}
