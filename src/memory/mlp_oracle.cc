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
 *                every line the MLC has looked up on an mlp_oracle_side side, for the
 *                reuse stats: whether REPL_MLP's marking has marked it, whether mode 1's
 *                control picked it, and when it was last looked up. It writes the misses
 *                and their marks for mlp_oracle_record, and lines misses up with a
 *                reference's for mlp_oracle_ref. memory.c does the forced hit itself.
 ***************************************************************************************/

#include <algorithm>
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string>
#include <unistd.h>
#include <unordered_map>
#include <vector>

extern "C" {
#include "globals/assert.h"
#include "globals/global_defs.h"
#include "globals/global_vars.h"
#include "globals/utils.h"

#include "core.param.h"
#include "general.param.h"
#include "memory/memory.param.h"

#include "memory/mlp_oracle.h"

#include "statistics.h"
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
/* MLP_ORACLE_<side>_*, in memory.stat.def's order. */
enum Oracle_Field {
  O_MARKED_LINES,
  O_MARKED_ACCESSES,
  O_CTRL_LINES,
  O_CTRL_LINES_MARKED,
  O_CTRL_ACCESSES,
  O_CTRL_NO_PICK,
  O_MISSES,
  O_MISSES_MARKED,
  O_FORCED,
  O_FORCED_REF_HIT,
  O_REF_MATCHED,
  O_REF_MATCHED_MARKED,
  O_REF_MATCHED_CTRL,
  O_REF_UNMATCHED,
  O_REF_SKIPPED,
  O_REF_MISSES,
  O_REF_MARKED,
  O_REF_MARKED_LINES,
  O_REF_CTRL,
  O_REF_CTRL_LINES,
  O_REF_CTRL_MARKED,
  O_FIELDS
};

struct Line {
  uns8 side = 0;           /* 0 instruction, 1 data, from the request that first named it */
  bool looked = false;     /* the MLC has looked it up, so stamp_* are set */
  bool seen = false;       /* a miss on it came back, or the MLC inserted it: later lookups are reuses */
  bool marked = false;     /* REPL_MLP's marking marked one of its fills */
  bool ctrl = false;       /* mode 1's control picked it */
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

/* A miss as mlp_oracle_record writes it: the line address, with the side in bit 0 and the mark in
   bit 1 (a line address leaves both clear), and the retired instructions at its lookup. */
struct Record {
  uint64_t key;
  uint64_t inst;
};

/* A miss that has not come back yet. */
struct Pending {
  Counter inst;
  uns8 side;
};

/* A miss of the reference, as replayed. */
struct Ref_Miss {
  Addr line;
  Counter inst;
  uns8 side;
  bool marked;
  bool ctrl;   /* mode 3's draw took it */
  bool chosen; /* this run forces a miss that lines up with it */
};

/* A line's misses in the reference: Core::ref[next, end) are the ones no miss has lined up with or
   passed yet. */
struct Ref_Line {
  uns next;
  uns end;
  bool forced = false; /* the last miss that lined up with one of them was forced */
};

struct Core {
  bool init = false;
  std::unordered_map<Addr, Line> lines;
  std::vector<Addr> pool[SIDES];             /* every seen line, to pick control lines from */
  std::vector<Counter> set_fills;            /* MLC inserts per set */
  Counter accesses = 0;                      /* MLC lookups, writebacks and merges aside */
  Next next[SIDES + 1];                      /* per side, and over both */
  std::vector<Addr> ctrl_set[SIDES];         /* mode 1's control lines */
  Counter marked_lines[SIDES] = {};          /* lines marked */
  Counter marked_accesses[SIDES] = {};       /* lookups of lines since they were marked */
  Counter ctrl_accesses[SIDES] = {};         /* and since they were picked */
  uint64_t rng = 0;                          /* mode 1's control */
  uint64_t ref_rng = 0;                      /* mode 3's draw */
  std::unordered_map<Addr, Pending> pending; /* misses out, by line */
  FILE* record = nullptr;
  bool replay = false;
  std::vector<Ref_Miss> ref; /* by line, then by retired instructions */
  std::unordered_map<Addr, Ref_Line> ref_lines;
};

std::vector<Core> cores;

/* xorshift64*, fixed seed: the same run picks the same lines in every build mode. */
uint64_t rand64(uint64_t& state) {
  state ^= state >> 12;
  state ^= state << 25;
  state ^= state >> 27;
  return state * 2685821657736338717ULL;
}

uns side_index(uns side_bit) {
  return side_bit == MLP_SIDE_INSTR ? 0 : 1;
}

uns side_bit_of(uns side) {
  return side ? MLP_SIDE_DATA : MLP_SIDE_INSTR;
}

void oracle_stat(uns proc_id, uns side, uns field) {
  STAT_EVENT(proc_id, MLP_ORACLE_I_MARKED_LINES + side * O_FIELDS + field);
}

void oracle_inc(uns proc_id, uns side, uns field, Counter inc) {
  INC_STAT_EVENT(proc_id, MLP_ORACLE_I_MARKED_LINES + side * O_FIELDS + field, inc);
}

void hist(uns proc_id, uns first, Counter dist) {
  uns bucket = dist ? 64 - __builtin_clzll(dist) : 0;
  STAT_EVENT(proc_id, first + MIN2(bucket, REUSE_BUCKETS - 1));
}

std::string record_name(uns proc_id) {
  return "mlp_oracle_misses." + std::to_string(proc_id) + ".bin";
}

std::vector<std::string> path_parts(const std::string& path) {
  std::vector<std::string> parts;
  size_t pos = 0;
  while (pos < path.size()) {
    size_t slash = path.find('/', pos);
    if (slash == std::string::npos)
      slash = path.size();
    if (slash > pos)
      parts.push_back(path.substr(pos, slash - pos));
    pos = slash + 1;
  }
  return parts;
}

/* The reference's record for this core, per mlp_oracle_ref: a directory given outright, or this
   run's output_dir with as many components as mlp_oracle_ref has swapped for them, nearest the
   end first, wherever that names a recorded run. */
std::string ref_path(uns proc_id) {
  std::string ref = MLP_ORACLE_REF;
  std::string name = record_name(proc_id);
  if (ref[0] == '/' || ref[0] == '.')
    return ref + "/" + name;
  char own[PATH_MAX];
  if (!realpath(OUTPUT_DIR, own))
    FATAL_ERROR(proc_id, "mlp_oracle_ref: cannot resolve output_dir %s\n", OUTPUT_DIR);
  std::vector<std::string> dirs = path_parts(own), swap = path_parts(ref);
  for (size_t at = dirs.size() >= swap.size() ? dirs.size() - swap.size() + 1 : 0; at-- > 0;) {
    std::string path;
    for (size_t i = 0; i < at; i++)
      path += "/" + dirs[i];
    for (const std::string& part : swap)
      path += "/" + part;
    for (size_t i = at + swap.size(); i < dirs.size(); i++)
      path += "/" + dirs[i];
    path += "/" + name;
    if (access(path.c_str(), R_OK) == 0)
      return path;
  }
  FATAL_ERROR(proc_id, "mlp_oracle_ref: no %s for %s in any run that swapping in %s names\n", name.c_str(), own,
              ref.c_str());
  return std::string();
}

/* Mode 3's draw on one side of the reference (see mlp_oracle_ctrl_lines). */
void draw_ctrl(Core& c, uns proc_id, uns side) {
  std::vector<uns> idx;   /* the side's misses, as indices into c.ref, line by line */
  std::vector<uns> start; /* where in idx each line's begin; one past the last at the end */
  Counter marked = 0, marked_lines = 0;
  Flag line_marked = FALSE;
  for (uns i = 0; i < c.ref.size(); i++) {
    const Ref_Miss& m = c.ref[i];
    if (m.side != side)
      continue;
    if (idx.empty() || c.ref[idx.back()].line != m.line) {
      start.push_back(idx.size());
      line_marked = FALSE;
    }
    if (m.marked) {
      marked++;
      marked_lines += !line_marked;
      line_marked = TRUE;
    }
    idx.push_back(i);
  }
  uns lines = start.size();
  start.push_back(idx.size());

  std::vector<uns> drawn; /* misses the control keeps */
  std::vector<uns> rest;  /* misses it may keep, to fill up to the marked count */
  if (MLP_ORACLE_CTRL_LINES) {
    std::vector<uns> avail(lines);
    for (uns l = 0; l < lines; l++)
      avail[l] = l;
    uns choices = MAX2(MLP_ORACLE_CTRL_CHOICES, 1);
    int64_t total = 0; /* misses on the lines drawn so far */
    for (Counter j = 0; j < marked_lines && !avail.empty(); j++) {
      /* What this line should bring the total up by, times marked_lines: to its share,
         (j + 1) / marked_lines of the marked misses. */
      int64_t need = (int64_t)(marked * (j + 1)) - total * (int64_t)marked_lines;
      uns best = 0;
      int64_t best_err = INT64_MAX;
      for (uns draw = 0; draw < choices; draw++) {
        uns k = rand64(c.ref_rng) % avail.size();
        int64_t n = start[avail[k] + 1] - start[avail[k]];
        int64_t err = llabs(n * (int64_t)marked_lines - need);
        if (err < best_err) {
          best_err = err;
          best = k;
        }
      }
      uns l = avail[best];
      avail[best] = avail.back();
      avail.pop_back();
      /* The line keeps one of its misses, so the trim below cannot drop it. */
      uns keep = start[l] + rand64(c.ref_rng) % (start[l + 1] - start[l]);
      drawn.push_back(idx[keep]);
      for (uns j = start[l]; j < start[l + 1]; j++)
        if (j != keep)
          rest.push_back(idx[j]);
      total += start[l + 1] - start[l];
    }
  } else {
    rest = idx;
  }
  /* Fill up to `marked` with a random subset of the rest. */
  uns want = marked > drawn.size() ? marked - drawn.size() : 0;
  if (rest.size() > want) {
    for (uns i = 0; i < want; i++)
      std::swap(rest[i], rest[i + rand64(c.ref_rng) % (rest.size() - i)]);
    rest.resize(want);
  }
  drawn.insert(drawn.end(), rest.begin(), rest.end());
  for (uns i : drawn)
    c.ref[i].ctrl = true;

  Counter ctrl_lines = 0, ctrl_marked = 0;
  for (uns l = 0; l < lines; l++) {
    Flag any = FALSE;
    for (uns j = start[l]; j < start[l + 1]; j++) {
      const Ref_Miss& m = c.ref[idx[j]];
      any |= m.ctrl;
      ctrl_marked += m.ctrl && m.marked;
    }
    ctrl_lines += any;
  }
  oracle_inc(proc_id, side, O_REF_MISSES, idx.size());
  oracle_inc(proc_id, side, O_REF_MARKED, marked);
  oracle_inc(proc_id, side, O_REF_MARKED_LINES, marked_lines);
  oracle_inc(proc_id, side, O_REF_CTRL, drawn.size());
  oracle_inc(proc_id, side, O_REF_CTRL_LINES, ctrl_lines);
  oracle_inc(proc_id, side, O_REF_CTRL_MARKED, ctrl_marked);
}

/* Reads the reference (mlp_oracle_ref), draws mode 3's control from it, and chooses the misses
   this run forces. */
void load_ref(Core& c, uns proc_id) {
  std::string path = ref_path(proc_id);
  FILE* file = fopen(path.c_str(), "rb");
  if (!file)
    FATAL_ERROR(proc_id, "mlp_oracle_ref: cannot open %s\n", path.c_str());
  std::vector<Record> records;
  Record buf[4096];
  for (size_t got; (got = fread(buf, sizeof(Record), 4096, file)) > 0;)
    records.insert(records.end(), buf, buf + got);
  Flag torn = ferror(file) || ftell(file) % sizeof(Record);
  fclose(file);
  if (torn)
    FATAL_ERROR(proc_id, "mlp_oracle_ref: %s is not a whole number of records\n", path.c_str());

  c.ref.reserve(records.size());
  for (const Record& r : records)
    c.ref.push_back({(Addr)(r.key & ~(uint64_t)3), (Counter)r.inst, (uns8)(r.key & 1), (r.key & 2) != 0, false, false});
  std::stable_sort(c.ref.begin(), c.ref.end(), [](const Ref_Miss& a, const Ref_Miss& b) {
    return a.line != b.line ? a.line < b.line : a.inst < b.inst;
  });
  for (uns i = 0; i < c.ref.size(); i++) {
    if (i == 0 || c.ref[i].line != c.ref[i - 1].line)
      c.ref_lines[c.ref[i].line] = {i, i};
    c.ref_lines[c.ref[i].line].end = i + 1;
  }
  for (uns side = 0; side < SIDES; side++)
    draw_ctrl(c, proc_id, side);
  for (Ref_Miss& m : c.ref)
    m.chosen = (side_bit_of(m.side) & MLP_ORACLE_SIDE) && (MLP_ORACLE == 2   ? m.marked
                                                           : MLP_ORACLE == 3 ? m.ctrl
                                                                             : false);
  c.replay = true;
}

Core& core_of(Cache* mlc, uns proc_id) {
  if (cores.empty())
    cores.resize(NUM_CORES);
  Core& c = cores[proc_id];
  if (!c.init) {
    c.init = true;
    ASSERTM(proc_id, MLP_ORACLE <= 4, "mlp_oracle %u is not 0 to 4\n", MLP_ORACLE);
    ASSERTM(proc_id, MLP_ORACLE_SIDE >= 1 && MLP_ORACLE_SIDE <= 3, "mlp_oracle_side %u is not 1 to 3\n",
            MLP_ORACLE_SIDE);
    ASSERTM(proc_id, !MLP_ORACLE_RECORD || MLP_ORACLE == 1, "mlp_oracle_record needs mlp_oracle 1\n");
    ASSERTM(proc_id, (MLP_ORACLE != 2 && MLP_ORACLE != 3) || MLP_ORACLE_REF,
            "mlp_oracle %u replays a reference: set mlp_oracle_ref\n", MLP_ORACLE);
    ASSERTM(proc_id, !MLP_ORACLE_REF || MLP_ORACLE <= 3, "mlp_oracle_ref needs mlp_oracle 1 to 3\n");
    c.set_fills.assign(mlc->num_sets, 0);
    for (Next& next : c.next)
      next.set_last.assign(mlc->num_sets, MAX_CTR);
    c.rng = 0x9E3779B97F4A7C15ULL * (proc_id + 1);
    c.ref_rng = 0xD1B54A32D192ED03ULL * (proc_id + 1);
    if (MLP_ORACLE_RECORD) {
      std::string path = std::string(OUTPUT_DIR) + "/" + record_name(proc_id);
      c.record = fopen(path.c_str(), "wb");
      if (!c.record)
        FATAL_ERROR(proc_id, "mlp_oracle_record: cannot write %s\n", path.c_str());
    }
    if (MLP_ORACLE_REF)
      load_ref(c, proc_id);
  }
  return c;
}

Line& line_of(Core& c, Addr line_addr, uns side) {
  auto ins = c.lines.try_emplace(line_addr);
  if (ins.second)
    ins.first->second.side = side;
  return ins.first->second;
}

/* Later lookups of the line are reuses, and the control can pick it. */
void see(Core& c, Line& line, Addr line_addr) {
  if (line.seen)
    return;
  line.seen = true;
  c.pool[line.side].push_back(line_addr);
}

/* One more line for mode 1's control on this side, at random from the side's seen lines that are
   not picked yet: of mlp_oracle_ctrl_choices drawn, the one with the most accesses so far while
   the side's control lines (since they were picked) trail its marked lines (since they were
   marked) in accesses, and the fewest otherwise. */
Flag pick_ctrl_line(Core& c, uns proc_id, uns side) {
  std::vector<Addr>& pool = c.pool[side];
  uns choices = MAX2(MLP_ORACLE_CTRL_CHOICES, 1);
  Flag want_hot = c.ctrl_accesses[side] < c.marked_accesses[side];
  Line* best = nullptr;
  Addr best_addr = 0;
  for (uns draw = 0, got = 0; got < choices && draw < 16 * choices && !pool.empty(); draw++) {
    Addr addr = pool[rand64(c.rng) % pool.size()];
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
  best->ctrl = true;
  c.ctrl_set[side].push_back(best_addr);
  oracle_stat(proc_id, side, O_CTRL_LINES);
  if (best->marked)
    oracle_stat(proc_id, side, O_CTRL_LINES_MARKED);
  return TRUE;
}

/* Mode 1: keep the side's control as large as its marked set. */
void balance_ctrl(Core& c, uns proc_id, uns side) {
  if (MLP_ORACLE != 1)
    return;
  while (c.ctrl_set[side].size() < c.marked_lines[side] && pick_ctrl_line(c, proc_id, side)) {
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

/* The line-level tracking of a lookup on an mlp_oracle_side side. Returns whether mode 4 forces
   it to hit. */
Flag track_lookup(Core& c, uns proc_id, Addr line_addr, uns set, uns side, Flag hit) {
  Line& line = line_of(c, line_addr, side);
  if (line.looked)
    record_reuse(c, proc_id, line, set, hit);
  line.looked = true;
  line.stamp_fills = c.set_fills[set];
  line.stamp_cycle = cycle_count;
  if (!line.seen)
    return FALSE; /* its first miss has not come back: no reuse yet */
  line.accesses++;
  if (line.marked) {
    c.marked_accesses[side]++;
    oracle_stat(proc_id, side, O_MARKED_ACCESSES);
  }
  if (line.ctrl) {
    c.ctrl_accesses[side]++;
    oracle_stat(proc_id, side, O_CTRL_ACCESSES);
  }
  return MLP_ORACLE == 4 && !hit;
}

/* Lines a miss up with the reference's next miss to its line that no miss has lined up with or
   passed, and returns whether this run forces it. */
Flag replay_miss(Core& c, uns proc_id, Addr line_addr, uns side) {
  auto it = c.ref_lines.find(line_addr);
  if (it == c.ref_lines.end()) {
    oracle_stat(proc_id, side, O_REF_UNMATCHED);
    return FALSE;
  }
  Ref_Line& rl = it->second;
  Counter now = inst_count[proc_id];
  Counter slack = MLP_ORACLE_REF_SLACK;
  while (rl.next < rl.end && c.ref[rl.next].inst + slack < now) {
    oracle_stat(proc_id, c.ref[rl.next].side, O_REF_SKIPPED);
    rl.next++;
  }
  if (rl.next == rl.end || c.ref[rl.next].inst > now + slack) {
    oracle_stat(proc_id, side, O_REF_UNMATCHED);
    /* mlp_oracle_install 0: a forced hit left the line out of the MLC, but it hit here in the
       reference. */
    if (rl.forced && !MLP_ORACLE_INSTALL) {
      oracle_stat(proc_id, side, O_FORCED_REF_HIT);
      return TRUE;
    }
    return FALSE;
  }
  const Ref_Miss& m = c.ref[rl.next++];
  if (now < m.inst)
    STAT_EVENT(proc_id, MLP_ORACLE_REF_EARLY);
  else if (now > m.inst)
    STAT_EVENT(proc_id, MLP_ORACLE_REF_LATE);
  hist(proc_id, MLP_ORACLE_REF_OFFSET_0, now < m.inst ? m.inst - now : now - m.inst);
  oracle_stat(proc_id, side, O_REF_MATCHED);
  if (m.marked)
    oracle_stat(proc_id, side, O_REF_MATCHED_MARKED);
  if (m.ctrl)
    oracle_stat(proc_id, side, O_REF_MATCHED_CTRL);
  rl.forced = m.chosen;
  return m.chosen;
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
  /* A miss on a line already on its way waits for it: not a lookup of its own. */
  if (!hit && in_flight)
    return FALSE;
  uns proc_id = req->proc_id;
  Core& c = core_of(mlc, proc_id);
  c.accesses++;
  uns side_bit = mlp_req_side(req);
  uns side = side_index(side_bit);
  Flag covered = (side_bit & MLP_ORACLE_SIDE) != 0;
  Addr tag, line_addr;
  uns set = ext_cache_index(mlc, req->addr, &tag, &line_addr);
  ASSERT(proc_id, !(line_addr & 3));

  Flag force = FALSE;
  if (!hit && c.replay)
    force = replay_miss(c, proc_id, line_addr, side) && covered;
  if (covered)
    force |= track_lookup(c, proc_id, line_addr, set, side, hit);
  if (hit)
    return FALSE;
  if (covered)
    oracle_stat(proc_id, side, O_MISSES);
  if (force) {
    oracle_stat(proc_id, side, O_FORCED);
    STAT_EVENT(proc_id, MLP_ORACLE_FORCED_HIT);
    if (mem_req_type_is_demand(req->type))
      STAT_EVENT(proc_id, MLP_ORACLE_FORCED_HIT_DEMAND);
    return TRUE;
  }
  /* A real miss: the marking says what it is when it comes back. One still pending never came
     back as a miss of its own, so goes on record unmarked. */
  auto ins = c.pending.try_emplace(line_addr);
  if (!ins.second && c.record) {
    Record r = {line_addr | ins.first->second.side, ins.first->second.inst};
    fwrite(&r, sizeof r, 1, c.record);
  }
  ins.first->second = {inst_count[proc_id], (uns8)side};
  return FALSE;
}

void mlp_oracle_miss_return(Cache* mlc, Mem_Req* req, uns marked_sides) {
  uns proc_id = req->proc_id;
  Core& c = core_of(mlc, proc_id);
  Addr tag, line_addr;
  ext_cache_index(mlc, req->addr, &tag, &line_addr);
  /* The first return for the line's miss settles its mark: for the record, and for MISSES_MARKED. */
  auto it = c.pending.find(line_addr);
  if (it != c.pending.end()) {
    uns side = it->second.side;
    Flag marked = (marked_sides & side_bit_of(side)) != 0;
    if (c.record) {
      Record r = {line_addr | side | (marked ? 2u : 0u), it->second.inst};
      fwrite(&r, sizeof r, 1, c.record);
    }
    if (marked && (side_bit_of(side) & MLP_ORACLE_SIDE))
      oracle_stat(proc_id, side, O_MISSES_MARKED);
    c.pending.erase(it);
  }

  uns side_bit = mlp_req_side(req);
  if (!(side_bit & MLP_ORACLE_SIDE))
    return;
  uns side = side_index(side_bit);
  Line& line = line_of(c, line_addr, side);
  see(c, line, line_addr);
  if ((marked_sides & side_bit) && !line.marked) {
    line.marked = true;
    c.marked_lines[side]++;
    oracle_stat(proc_id, side, O_MARKED_LINES);
    balance_ctrl(c, proc_id, side);
  }
}

void mlp_oracle_insert(Cache* mlc, Mem_Req* req) {
  Core& c = core_of(mlc, req->proc_id);
  Addr tag, line_addr;
  uns set = ext_cache_index(mlc, req->addr, &tag, &line_addr);
  c.set_fills[set]++;
  uns side_bit = mlp_req_side(req);
  if (side_bit & MLP_ORACLE_SIDE)
    see(c, line_of(c, line_addr, side_index(side_bit)), line_addr);
}
