/*
 * Copyright 2026 University of California Santa Cruz
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
 * File         : memory/mlp_oracle.cc
 * Author       : Litz Lab
 * Date         : 11/2026
 * Description  : mlp_oracle (see mlp_oracle.h and memory.param.def). Per core, it keeps
 *                every line the MLC has looked up on an mlp_oracle_side side: whether
 *                REPL_MLP's marking has marked it, whether the random control picked it,
 *                and when it was last looked up, for the reuse distances. memory.c does
 *                the forced hit itself.
 ***************************************************************************************/

#include <unordered_map>
#include <vector>

extern "C" {
#include "globals/assert.h"
#include "globals/global_defs.h"
#include "globals/global_vars.h"
#include "globals/utils.h"

#include "core.param.h"
#include "memory/memory.param.h"

#include "statistics.h"

#include "memory/mlp_oracle.h"
}

namespace {

/* Histograms: bucket 0 is a distance of 0, bucket b in 1..24 is [2^(b-1), 2^b), and the last
   bucket is everything from 2^24 up. */
constexpr uns REUSE_BUCKETS = 26;
/* Per-side stats and state: 0 instruction, 1 data. */
constexpr uns SIDES = 2;
/* Line classes for the reuse stats. MARKED and UNMARKED split the lines of a side; CTRL counts
   the control's lines again, whichever of the two they are in. */
enum Reuse_Class {
  MARKED,
  UNMARKED,
  CTRL,
  CLASSES
};
/* MLP_REUSE_<side>_<class>_*: samples, those that hit, and the sums of their distances. */
enum Reuse_Field {
  R_N,
  R_HIT,
  R_FILLS,
  R_CYCLES,
  R_FIELDS
};
/* MLP_REUSE_NEXT_<side>_*: gaps, their sums in cycles and MLC lookups, and the per-set gaps. */
enum Next_Field {
  N_N,
  N_CYCLES,
  N_ACCESSES,
  N_FILLS_N,
  N_FILLS,
  N_FIELDS
};
/* MLP_ORACLE_<side>_*. */
enum Oracle_Field {
  O_MARKED_LINES,
  O_MARKED_ACCESSES,
  O_CTRL_LINES,
  O_CTRL_LINES_MARKED,
  O_CTRL_ACCESSES,
  O_CTRL_NO_PICK,
  O_SHADOW_HIT,
  O_MARK_LAPSED,
  O_CTRL_LAPSED,
  O_CTRL_DROPPED,
  O_FIELDS
};

struct Line {
  uns8 side = 0;           /* 0 instruction, 1 data, from the request that first named it */
  bool looked = false;     /* the MLC has looked it up, so stamp_* are set */
  bool seen = false;       /* a miss on it came back, or the MLC inserted it: later lookups are reuses */
  bool marked = false;     /* REPL_MLP's marking marked one of its fills */
  bool ctrl = false;       /* the control picked it */
  uns ctrl_idx = 0;        /* where in its side's ctrl_set, while ctrl */
  Counter accesses = 0;    /* lookups since it was seen */
  Counter stamp_fills = 0; /* its set's fill count at its last lookup */
  Counter stamp_cycle = 0; /* and the cycle */
};

/* The previous lookup of a marked line, for the gap to the next one. */
struct Next {
  Counter last_cycle = MAX_CTR;
  Counter last_access = 0;
  std::vector<Counter> set_last; /* set_fills at the set's last one; MAX_CTR: none */
};

struct Core {
  bool init = false;
  std::unordered_map<Addr, Line> lines;
  std::vector<Addr> pool[SIDES];       /* every seen line, to pick control lines from */
  std::vector<Counter> set_fills;      /* MLC inserts per set */
  Counter accesses = 0;                /* MLC lookups, writebacks and merges aside */
  Next next[SIDES + 1];                /* per side, and over both */
  std::vector<Addr> ctrl_set[SIDES];   /* the control's lines */
  Counter marked_live[SIDES] = {};     /* lines marked now */
  Counter marked_accesses[SIDES] = {}; /* lookups of lines since they were marked */
  Counter ctrl_accesses[SIDES] = {};   /* and since they were picked */
  uint64_t rng = 0;
};

std::vector<Core> cores;

Core& core_of(Cache* mlc, uns proc_id) {
  if (cores.empty())
    cores.resize(NUM_CORES);
  Core& c = cores[proc_id];
  if (!c.init) {
    c.init = true;
    c.set_fills.assign(mlc->num_sets, 0);
    for (Next& next : c.next)
      next.set_last.assign(mlc->num_sets, MAX_CTR);
    c.rng = 0x9E3779B97F4A7C15ULL * (proc_id + 1);
  }
  return c;
}

/* xorshift64*, fixed seed: the same run picks the same lines in every build mode. */
uint64_t rand64(Core& c) {
  c.rng ^= c.rng >> 12;
  c.rng ^= c.rng << 25;
  c.rng ^= c.rng >> 27;
  return c.rng * 2685821657736338717ULL;
}

uns side_index(uns side_bit) {
  return side_bit == MLP_SIDE_INSTR ? 0 : 1;
}

void oracle_stat(uns proc_id, uns side, uns field) {
  STAT_EVENT(proc_id, MLP_ORACLE_I_MARKED_LINES + side * O_FIELDS + field);
}

void hist(uns proc_id, uns first, Counter dist) {
  uns bucket = dist ? 64 - __builtin_clzll(dist) : 0;
  STAT_EVENT(proc_id, first + MIN2(bucket, REUSE_BUCKETS - 1));
}

Line& line_of(Core& c, Cache* mlc, Mem_Req* req, uns* set, Addr* line_addr) {
  Addr tag;
  *set = ext_cache_index(mlc, req->addr, &tag, line_addr);
  auto ins = c.lines.try_emplace(*line_addr);
  if (ins.second)
    ins.first->second.side = side_index(mlp_req_side(req));
  return ins.first->second;
}

/* Later lookups of the line are reuses, and the control can pick it. */
void see(Core& c, Line& line, Addr line_addr) {
  if (line.seen)
    return;
  line.seen = true;
  c.pool[line.side].push_back(line_addr);
}

void add_ctrl(Core& c, Line& line, Addr line_addr) {
  std::vector<Addr>& set = c.ctrl_set[line.side];
  line.ctrl = true;
  line.ctrl_idx = set.size();
  set.push_back(line_addr);
}

void drop_ctrl(Core& c, Line& line) {
  std::vector<Addr>& set = c.ctrl_set[line.side];
  Addr last = set.back();
  set[line.ctrl_idx] = last;
  c.lines[last].ctrl_idx = line.ctrl_idx;
  set.pop_back();
  line.ctrl = false;
}

/* One more control line on this side, at random from the side's seen lines that are not picked
   yet. Matching accesses, it draws mlp_oracle_ctrl_choices of them and takes the one with the most
   accesses so far while the side's control lines trail its marked lines in accesses, and the
   fewest otherwise. */
Flag pick_ctrl_line(Core& c, uns proc_id, uns side) {
  std::vector<Addr>& pool = c.pool[side];
  uns choices = MLP_ORACLE_CTRL_ACCESSES ? MAX2(MLP_ORACLE_CTRL_CHOICES, 1) : 1;
  Flag want_hot = c.ctrl_accesses[side] < c.marked_accesses[side];
  Line* best = nullptr;
  Addr best_addr = 0;
  for (uns draw = 0, got = 0; got < choices && draw < 16 * choices && !pool.empty(); draw++) {
    Addr addr = pool[rand64(c) % pool.size()];
    Line& cand = c.lines[addr];
    if (cand.ctrl)
      continue;
    got++;
    if (!best || (want_hot ? cand.accesses > best->accesses : cand.accesses < best->accesses)) {
      best = &cand;
      best_addr = addr;
    }
  }
  if (!best) {
    oracle_stat(proc_id, side, O_CTRL_NO_PICK);
    return FALSE;
  }
  add_ctrl(c, *best, best_addr);
  oracle_stat(proc_id, side, O_CTRL_LINES);
  if (best->marked)
    oracle_stat(proc_id, side, O_CTRL_LINES_MARKED);
  return TRUE;
}

/* mlp_oracle 1 and 3: keep the side's control as large as its marked set. Picks while it is
   smaller; while it is larger, which only a lapsed mark makes it, drops one of its lines at
   random. */
void balance_ctrl(Core& c, uns proc_id, uns side) {
  if (MLP_ORACLE != 1 && MLP_ORACLE != 3)
    return;
  std::vector<Addr>& set = c.ctrl_set[side];
  while (set.size() < c.marked_live[side] && pick_ctrl_line(c, proc_id, side)) {
  }
  while (set.size() > c.marked_live[side]) {
    drop_ctrl(c, c.lines[set[rand64(c) % set.size()]]);
    oracle_stat(proc_id, side, O_CTRL_DROPPED);
  }
}

/* The gap from the previous lookup of a marked line (on this side, or either) to this one. */
void next_gap(Core& c, uns proc_id, uns v, uns set) {
  Next& next = c.next[v];
  uns base = MLP_REUSE_NEXT_I_N + v * N_FIELDS;
  uns hist_base = MLP_REUSE_NEXT_I_CYCLES_0 + v * 2 * REUSE_BUCKETS;
  if (next.last_cycle != MAX_CTR) {
    Counter cycles = cycle_count - next.last_cycle;
    STAT_EVENT(proc_id, base + N_N);
    INC_STAT_EVENT(proc_id, base + N_CYCLES, cycles);
    INC_STAT_EVENT(proc_id, base + N_ACCESSES, c.accesses - next.last_access);
    hist(proc_id, hist_base, cycles);
  }
  next.last_cycle = cycle_count;
  next.last_access = c.accesses;
  if (next.set_last[set] != MAX_CTR) {
    Counter fills = c.set_fills[set] - next.set_last[set];
    STAT_EVENT(proc_id, base + N_FILLS_N);
    INC_STAT_EVENT(proc_id, base + N_FILLS, fills);
    hist(proc_id, hist_base + REUSE_BUCKETS, fills);
  }
  next.set_last[set] = c.set_fills[set];
}

void reuse_sample(uns proc_id, uns side, uns cls, Flag hit, Counter fills, Counter cycles) {
  uns base = MLP_REUSE_I_MARKED_N + (side * CLASSES + cls) * R_FIELDS;
  STAT_EVENT(proc_id, base + R_N);
  if (hit)
    STAT_EVENT(proc_id, base + R_HIT);
  INC_STAT_EVENT(proc_id, base + R_FILLS, fills);
  INC_STAT_EVENT(proc_id, base + R_CYCLES, cycles);
  hist(proc_id, MLP_REUSE_I_MARKED_FILLS_0 + (side * CLASSES + cls) * REUSE_BUCKETS, fills);
}

/* A lookup of a line looked up before: how far it is from that lookup, by class, and for a
   marked line, from the previous lookup of any marked line. */
void record_reuse(Core& c, uns proc_id, Line& line, uns set, Flag hit) {
  Counter fills = c.set_fills[set] - line.stamp_fills;
  Counter cycles = cycle_count - line.stamp_cycle;
  reuse_sample(proc_id, line.side, line.marked ? MARKED : UNMARKED, hit, fills, cycles);
  if (line.ctrl)
    reuse_sample(proc_id, line.side, CTRL, hit, fills, cycles);
  if (line.marked) {
    next_gap(c, proc_id, line.side, set);
    next_gap(c, proc_id, SIDES, set);
  }
}

}  // namespace

uns mlp_req_side(Mem_Req* req) {
  switch (req->type) {
    case MRT_IFETCH:
    case MRT_IPRF:
    case MRT_UOCPRF:
    case MRT_FDIPPRFON:
    case MRT_FDIPPRFOFF:
    case MRT_FDIPPRFALT:
      return MLP_SIDE_INSTR;
    default:
      return mem_req_is_type(req, MRT_IFETCH) ? MLP_SIDE_INSTR : MLP_SIDE_DATA;
  }
}

Flag mlp_oracle_lookup(Cache* mlc, Mem_Req* req, Flag hit, Flag in_flight) {
  ASSERTM(req->proc_id, MLP_ORACLE <= 4, "mlp_oracle %u is not 0 to 4\n", MLP_ORACLE);
  ASSERTM(req->proc_id, MLP_ORACLE_SIDE >= 1 && MLP_ORACLE_SIDE <= 3, "mlp_oracle_side %u is not 1 to 3\n",
          MLP_ORACLE_SIDE);
  /* A miss on a line already on its way waits for it: not a lookup of its own. */
  if (!hit && in_flight)
    return FALSE;
  Core& c = core_of(mlc, req->proc_id);
  c.accesses++;
  if (!(mlp_req_side(req) & MLP_ORACLE_SIDE))
    return FALSE;
  uns set;
  Addr line_addr;
  Line& line = line_of(c, mlc, req, &set, &line_addr);
  Counter since = line.looked ? c.set_fills[set] - line.stamp_fills : 0;
  if (line.looked)
    record_reuse(c, req->proc_id, line, set, hit);
  line.looked = true;
  line.stamp_fills = c.set_fills[set];
  line.stamp_cycle = cycle_count;
  if (!line.seen)
    return FALSE; /* its first miss has not come back: no reuse yet */

  uns side = line.side;
  /* mlp_oracle_window: a miss more than the window's fills after the line's last lookup is one
     REPL_MLP's protection would have let go. It is a real miss, the line loses its mark (and its
     control pick), and the miss's fill can mark it again. */
  Flag lapsed = !hit && MLP_ORACLE_WINDOW && since > MLP_ORACLE_WINDOW;
  if (lapsed && line.marked) {
    line.marked = false;
    c.marked_live[side]--;
    oracle_stat(req->proc_id, side, O_MARK_LAPSED);
  }
  if (lapsed && line.ctrl) {
    drop_ctrl(c, line);
    oracle_stat(req->proc_id, side, O_CTRL_LAPSED);
  }
  line.accesses++;
  if (line.marked) {
    c.marked_accesses[side]++;
    oracle_stat(req->proc_id, side, O_MARKED_ACCESSES);
  }
  if (line.ctrl) {
    c.ctrl_accesses[side]++;
    oracle_stat(req->proc_id, side, O_CTRL_ACCESSES);
  }
  if (hit)
    return FALSE;

  if (line.marked)
    oracle_stat(req->proc_id, side, O_SHADOW_HIT);
  Flag force = (MLP_ORACLE == 2 && line.marked) || (MLP_ORACLE == 3 && line.ctrl) || (MLP_ORACLE == 4 && !lapsed);
  if (force) {
    STAT_EVENT(req->proc_id, MLP_ORACLE_FORCED_HIT);
    if (mem_req_type_is_demand(req->type))
      STAT_EVENT(req->proc_id, MLP_ORACLE_FORCED_HIT_DEMAND);
  }
  /* After this miss is decided, so a refill of the control cannot pick this line for it. */
  if (lapsed)
    balance_ctrl(c, req->proc_id, side);
  return force;
}

void mlp_oracle_miss_return(Cache* mlc, Mem_Req* req, uns marked_sides) {
  uns side_bit = mlp_req_side(req);
  if (!(side_bit & MLP_ORACLE_SIDE))
    return;
  Core& c = core_of(mlc, req->proc_id);
  uns set;
  Addr line_addr;
  Line& line = line_of(c, mlc, req, &set, &line_addr);
  see(c, line, line_addr);
  if ((marked_sides & side_bit) && !line.marked) {
    line.marked = true;
    c.marked_live[line.side]++;
    oracle_stat(req->proc_id, line.side, O_MARKED_LINES);
    balance_ctrl(c, req->proc_id, line.side);
  }
}

void mlp_oracle_insert(Cache* mlc, Mem_Req* req) {
  Core& c = core_of(mlc, req->proc_id);
  Addr tag, line_addr;
  uns set = ext_cache_index(mlc, req->addr, &tag, &line_addr);
  c.set_fills[set]++;
  if (!(mlp_req_side(req) & MLP_ORACLE_SIDE))
    return;
  see(c, line_of(c, mlc, req, &set, &line_addr), line_addr);
}
