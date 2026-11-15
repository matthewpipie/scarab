# Branch-feeder data marking (`mlp_data_branch`)

`--mlp_data_branch` adds a second way for REPL_MLP to mark an L2 (MLC) data fill: a fill is also marked if a branch counted as hard to predict read the waiting load's value. The rule is ORed with the data-bound marking in [repl-mlp.md](repl-mlp.md#data-marking). It is **off by default**, and with it off Scarab is cycle-identical to the base.

Summary of what was measured:

- **Cheat mode (perfect knowledge of which branches mispredict) is the upper bound for this signal, and it is small.** Across 6 suites, 213 workloads and 1,171 simpoints, its best suite geomean is +0.39% (Google).
- **Only 4 of 213 workloads gain 1% or more from it.** All have high mispredict rates or are cases the existing data rule already wins.
- **The realizable mode (TAGE confidence) tracks cheat where cheat helps,** with a rank correlation of 0.64 across workloads. It catches about 40% of mispredicts.
- **The rule marks 9 to 22 times fewer lines than the existing data-bound rule.** That is its one structural advantage: on Google at γd 64 the existing rule loses 0.77% (it pushes instruction lines out), while cheat gains 0.39%.
- **It does not reproduce the existing rule's wins.** On SPEC2006, those wins are thrash protection: only 2.7% of the lines the existing rule marks feed a mispredicting branch.
- **Not recommended.** Narrower uses that might still pay off are listed under [Where it might still help](#where-it-might-still-help).
- Building it turned up a frontend bug: Google traces had no flag dependencies, so conditional branches never waited on their compare. That is fixed in #44, which this PR builds on.

## Why we tried it

Two measurements suggested that protecting data lines pays off partly through branch mispredicts. Both were taken before #44, so their Google numbers come from the old frontend.

- **Where data γ's saved cycles go.** In the mlp_gamma sweep (golden_small, γd-only configs, simpoints more than 0.5% faster than LRU), each topdown bucket's share of the saved cycles was:

  | Suite | Bad speculation | Backend stall | Frontend |
  |---|---|---|---|
  | google | 119% | 48–50% | −67% |
  | datacenter | 56–82% | 43–54% | −10 to −26% |
  | spec2006 | 39–45% | 55–61% | 0% |
  | spec2017 / spec2026 | 12–19% | 82–91% | 0 to −3% |

  The number of mispredicts did not change. What shrank was the wrong-path time per mispredict: −9 to −13% on datacenter, −6 to −7% on spec2006. A load feeding a mispredicted branch keeps the frontend on the wrong path until it returns, so making it hit sooner shortens the wrong path.
- **Whether the existing data mark picks branch-feeding loads.** In the oracle study of #36 (google, 12 workloads, 35 simpoints, 50M warmup + 500M measured), forced L2 hits on marked data misses were compared with forced hits on the same number of randomly drawn misses:

  | Forced hits on | Cycles saved per hit | Of which bad speculation | Share | Wrong-path cycles per mispredict |
  |---|---|---|---|---|
  | random data misses | 34.3 | 14.8 | 43% | 70.1 → 61.6 |
  | marked data misses | 49.8 | 23.7 | 48% | 70.1 → 56.6 |
  | all data misses | 34.0 | 15.4 | 45% | 70.1 → 49.8 |

  The marked misses are costlier, but their bad-speculation share is about the same as random ones. So the existing mark does not single out loads that feed branches, and a mark that targets them directly had not been tried.

## How it works

**At rename** (`map_stage.c`, `mlp_branch_rename`), every op records a feed load:

- a load records itself;
- any other op records the youngest feed load among its register sources;
- a source whose producer or feed load has left the machine contributes nothing, since its data is back and its fill has happened.

This models a load-queue tag carried with each physical register. On x86 a conditional branch reads only the flags register, so a `load → cmp → jcc` chain reaches the load through the cmp.

**When a branch in scope counts as hard and has a feed load,** it sets a flag on that load. If the load is already waiting on a memory request, the flag is also set on the request; a request the load creates or joins later copies it (`memory.c`).

**At the MLC fill** (`mlp_data_marked`), the fill is marked if the data-bound rule marks it **or** the request carries the flag. A marked fill gets the usual `mlp_lin_data_lambda` boost.

| Flag | Values |
|---|---|
| `--mlp_data_branch` (default `0`) | `0` off. `1` conditional branches whose TAGE provider confidence (`op->bp_confidence`, `bp_mech tage64k`) is in the bitmask below. `2` **cheat, not implementable**: on-path conditional, indirect jump, indirect call and coroutine branches that the simulator knows will mispredict and recover at exec (it reads `off_path` and the oracle recovery point). |
| `--mlp_data_branch_conf` (default `0x3`) | Bitmask of the `bp_confidence` values counted as hard. The values come from the TAGE provider's counter: `1` weakest, `0` next weakest, `2` medium, `3` saturated (or a saturated bimodal entry). The default takes the two weakest. |
| `--mlp_lin_data_thresh 1.0` | Not new. It disables the data-bound rule (its ratio cannot exceed 1), which leaves the branch rule alone. |

The cost of a hardware version of mode 1:

- a load-queue index (7–8 bits) plus a valid bit per physical register;
- one bit per load-queue entry and one per MSHR;
- a youngest-of-sources select at rename.

The TAGE confidence is already known when the branch is predicted.

Limitations:

- **One tag per register.** It names only the youngest load in the chain. With pointer chasing (load A's value is load B's address), only B is flagged.
- **Timing.** The branch must be renamed before the load's L2 fill, or nothing is flagged.
- **Which access gets protected.** The flag comes from this access's branch, but the protection pays off on the line's next access.
- **Branch types.** TAGE gives a confidence only for conditional branches.

**Stats** (`memory.stat.def`):

| Stat | Counts |
|---|---|
| `MLP_DATA_FILL_{NONE,BOUND,BRANCH,BOTH}` | With the mode on: each data request judged at its MLC fill, by which rules mark it. |
| `MLP_BR_CONF_{0..3}_{RETIRED,MISPRED}` | Under every policy: retired conditional branches by TAGE confidence, and those that recovered at exec. |
| `MLP_BR_RET_{UNFED,FED}_{EASY,HARD}_{OK,MISP}` | With the mode on: retired branches in the mode's scope, by whether a load fed them at rename, whether they counted as hard, and whether they mispredicted. |

## Results

Common to all runs:

- golden_small, plain L2 LRU as the baseline;
- **data side only** (γi 0); the instruction side was never on;
- gains are IPC geomeans over workloads; a workload's IPC is the weight-averaged IPC of its simpoints, as `sci --visualize` computes it;
- "existing mark" is main's default data-bound rule (no-issue ROB-head cycles ≥ 16).

### Mode 0 changes nothing

Cycle counts with the mode off were identical to the base on every simpoint checked:

- 12 google simpoints, at both commits;
- 6 datacenter simpoints;
- for both plain LRU and REPL_MLP γd 16.

### Google and datacenter, top simpoint, γd 16

Google: 12 workloads, top simpoint, 10M warmup + 50M measured, on top of #44. Datacenter: 6 workloads (xgboost left out because it is slow), top simpoint, 10M + 10M.

| Config | google gain | google loads marked | google L2 instr misses vs LRU | datacenter gain |
|---|---|---|---|---|
| existing mark | +0.28% | 1.58M | +16.8% | +0.17% |
| existing OR TAGE | +0.28% | 1.68M | +18.1% | +0.17% |
| TAGE only | +0.08% | 0.23M | +2.4% | +0.17% |
| existing OR cheat | +0.31% | 1.65M | +17.7% | +0.21% |
| cheat only | +0.20% | 0.18M | +2.1% | +0.08% |

- On google, 84.5% of conditional branches name a load in flight at rename; on datacenter, 86%.
- TAGE confidence 0 or 1 flags 41% of google mispredicts and 45% of datacenter mispredicts. Of the flagged branches that a load feeds, 23% (google) and 15% (datacenter) actually mispredict.
- Both branch rules save mostly wrong-path time. Wrong-path cycles per mispredict on google went 110.4 → 109.6 with cheat only.

### SPEC2006 workloads the data γ helps

These are the 6 workloads that gained at least 1% from data γ alone in the mlp_gamma sweep: 33 simpoints (top 6 per workload where available), 10M + 10M.

| Workload | Existing γd 16 | Existing γd 64 | Cheat γd 16 | Cheat γd 64 | TAGE γd 64 |
|---|---|---|---|---|---|
| sphinx3 | +22.1% | +26.3% | +1.0% | +1.1% | +1.7% |
| mcf | +6.6% | +13.8% | −0.2% | −0.1% | +0.4% |
| gcc_7 | 0.0% | +1.8% | 0.0% | 0.0% | 0.0% |
| gcc_2 | +0.9% | +1.0% | +0.1% | +0.1% | +0.2% |
| gcc_4 | +1.0% | +0.8% | +0.1% | +0.1% | +0.3% |
| soplex | +0.3% | +0.1% | +0.6% | **+4.3%** | **+2.4%** |
| geomean | +4.9% | +6.9% | +0.3% | +0.9% | +0.8% |

- **Lines marked:** 2.54M by the existing rule, 160–208K by cheat.
- **Overlap:** in the ORed runs, 2.7% of the fills the existing rule marks also feed a mispredicting branch, and 34–36% of cheat's fills are also marked by the existing rule.
- **Where the gains come from:** the existing rule's gains split about evenly between bad speculation and backend stall; cheat's are 86–99% bad speculation.
- **These wins are thrash protection.** 90–99% of these workloads' mispredicts are load-fed, but they are rare (0.3–2.4 per 1,000 instructions, mcf 5.2).
- **soplex is the exception** at 8.8 mispredicts per 1,000 instructions. Cheat γd 64 gains +4.3% there. ORing it with the existing rule drops that to +1.0%, because the many existing-rule marks crowd out the few branch marks.
- At γd 64 the existing rule raises L2 instruction misses by 179% across these workloads. SPEC tolerates that; Google does not, as the next table shows.

### All benchmarks

6 suites, 213 workloads, 1,171 simpoints (the mlp_gamma sweep's top 6 per workload), 10M + 10M (google 10M + 50M), on top of #44. That is 7,026 sims, run on pakuchi and the cloudhost VMs.

| Suite | Existing γd 16 | Existing γd 64 | Cheat γd 16 | Cheat γd 64 | TAGE γd 64 |
|---|---|---|---|---|---|
| google (12 workloads) | +0.36% | −0.77% | +0.23% | +0.39% | +0.02% |
| datacenter (7) | +0.25% | +0.41% | +0.13% | +0.33% | +0.35% |
| spec2006 (55) | +0.39% | +0.20% | +0.03% | +0.10% | +0.09% |
| spec2017 (36) | +0.10% | −0.18% | +0.01% | +0.02% | 0.00% |
| spec2026 (90) | +0.26% | +0.13% | +0.01% | +0.01% | 0.00% |
| geekbench (13) | −0.08% | −0.20% | +0.02% | +0.01% | +0.01% |

Every workload where cheat or TAGE gained at least 1%:

| Workload | Best existing | Cheat γd 64 | TAGE γd 64 | Exec mispredicts per 1,000 instructions |
|---|---|---|---|---|
| spec2006 soplex | +0.3% | +4.3% | +2.4% | 8.8 |
| datacenter xgboost | +0.7% | +1.8% | +2.0% | 13.8 |
| google yankee | +2.6% | +2.2% | +1.2% | 5.9 |
| spec2006 sphinx3 | +26.3% | +1.1% | +1.7% | 0.9 |

Mean gain by the workload's mispredict rate:

| Mispredicts per 1,000 instructions | Workloads | Cheat γd 64 | TAGE γd 64 | Existing γd 64 |
|---|---|---|---|---|
| ≤ 1 | 79 | +0.01% | +0.02% | +0.05% |
| 1–2 | 52 | +0.02% | +0.02% | −0.08% |
| 2–4 | 42 | +0.05% | −0.02% | −0.29% |
| 4–8 | 30 | +0.13% | +0.05% | +0.54% |
| > 8 | 10 | +0.70% | +0.51% | +0.97% |

Loads marked, cheat γd 64 vs the existing rule at γd 64:

| Suite | Cheat | Existing |
|---|---|---|
| google | 0.86M | 7.6M |
| datacenter | 27K | 399K |
| spec2006 | 449K | 6.6M |
| spec2017 | 139K | 2.9M |
| spec2026 | 237K | 4.9M |
| geekbench | 30K | 666K |

### TAGE confidence as the hard-branch signal

Retired conditional branches with plain LRU, by `bp_confidence`:

| Value | Google: share of branches | Google: mispredict rate | Google: share of mispredicts | Datacenter: share of branches | Datacenter: mispredict rate | Datacenter: share of mispredicts |
|---|---|---|---|---|---|---|
| 1 (weakest) | 2.0% | 25.5% | 29.8% | 4.4% | 15.8% | 40.1% |
| 0 | 0.9% | 20.0% | 11.1% | 0.7% | 12.9% | 4.9% |
| 2 | 1.3% | 14.0% | 10.6% | 0.8% | 8.4% | 4.1% |
| 3 (saturated) | 95.8% | 0.86% | 48.5% | 94.1% | 0.93% | 50.8% |

About half of all mispredicts come from branches TAGE was most confident about. Those are probably overrides by the statistical corrector or loop predictor, and saturated bimodal entries. No confidence threshold can catch them.

## Where it might still help

- **Gate it on a mispredict-rate counter,** so it is on only in branchy phases like soplex and xgboost. The existing rule is noise or a loss in the 1–4 mispredicts-per-kilo-instruction range, where the branch rule is close to neutral.
- **Pair it with γi 64 on Google.** With γi 0, the existing data rule's capacity cost on instruction lines turns γd 64 into −0.77%. The branch rule's small footprint might not displace the instruction lines that γi protects. This has not been run.
- **Track the chain's root load,** not only the youngest. This would cover pointer chasing.

## Reproducing

```
# realizable, ORed with the existing data mark
--mlc_cache_repl_policy 9 --mlp_boost_in_fills 1 --mlp_lin_data_lambda 64 --mlp_data_branch 1
# branch rule alone (cheat)
--mlc_cache_repl_policy 9 --mlp_boost_in_fills 1 --mlp_lin_data_lambda 64 --mlp_data_branch 2 --mlp_lin_data_thresh 1.0
```

The scarab-infra descriptors (untracked, in the run checkout's `json/`) are:

- `mlp_dbr_smoke_{google,datacenter}_small` and `mlp_dbr_smoke2_google_small`
- `mlp_dbr_spec06_small`
- `mlp_dbr_all_<suite>_small`

They ran this branch's code at 6ae0176 and 00226e7.
