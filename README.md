# Vampires VS Werewolves — RL agent

Tournament client plus the training code that produced the neural policy.

Two things live here:

1. **`main.py`** — the client that talks to the professor's server and plays.
2. **`train.py`** — the training pipeline that produced `best.pt`.

They are deliberately separate. `main.py` imports no torch at module level, so
it still runs on a machine where torch is missing; in that case it plays the
heuristic instead of the network.

---

## Quick start

Install:

```bash
pip install -r requirements.txt
```

Play against a server (ip and port are always the **last two** arguments):

```bash
python main.py 127.0.0.1 5555
```

Run the tests:

```bash
python -m pytest -q
```

11 tests skip here: they need the professor's `.xml` maps, which are not in
this repo. Everything else runs.

Check the agent is ready to compete (builds a folder and plays a real game
against a mock server):

```bash
python tools/package_submission.py --checkpoint best.pt --out submission
```

---

## The two algorithms

### 1. Heuristic — the baseline and the safety net

**Name: one-ply greedy search with beam-search turn assembly over an intent
shortlist, scored by a weighted linear evaluation function.**

No minimax, no alpha-beta, no tree or graph library — the course forbids those,
and nothing here needs them.

A turn in this game is a *compound* action: any subset of our groups may move,
each wholly or in part. The space is far too large to enumerate, so:

1. **Intents.** Each group proposes at most 6 tactically meaningful moves, from
   six archetypes:

   | Intent | What it does |
   |---|---|
   | `convert` | step onto a house we outnumber, converting it for free |
   | `split` | divide a group so two houses fall in the same turn |
   | `attack` | step onto enemies we beat at the 1.5× bar |
   | `regroup` | merge toward a friendly group to clear that bar next turn |
   | `retreat` | step away from an enemy who could wipe us out |
   | `advance` | close on the best house we are nearer to than they are |

2. **Beam assembly.** A beam of width 8 stitches per-group intents into whole
   legal turns.

3. **Scoring.** Each completed turn is applied and the resulting board is scored
   by `vvw/evaluate.py`: a weighted linear sum of population, humans we can
   reach first, creatures standing in danger, and groups too small to be useful.

Distance is plain **Chebyshev** (`max(|dx|, |dy|)`). The board has no walls and
movement is eight-way, so the step count between two cells is exact arithmetic —
no pathfinding.

Entry point: `vvw/agents/heuristic.py` → `choose_turn(state, species)`

### 2. Neural policy — the trained model

**Network: a fully convolutional policy-value network, 78,210 parameters.**

The trunk is a conv stem plus 4 residual blocks, then `AdaptiveAvgPool2d(1)`.
Global pooling is what makes **one set of weights play every board size** from
10×10 to 50×50 without retraining or reshaping. Whole-board totals that pooling
would destroy (populations, who is ahead, turns left) are concatenated back in
as 5 scalars.

**Action space: candidate choice.** The policy does not emit a compound turn.
The same generator the heuristic uses proposes up to **16 legal turns**, and the
network picks one of them. This avoids an autoregressive head over
(source, destination, count, continue) with legality masks at every step.

The candidates are **shuffled** before the network sees them. Sorted by
heuristic score, "copy the heuristic" would just mean "always output index 0",
which a network can satisfy without ever reading a candidate's features.

Entry point: `vvw/agents/policy.py` → `PolicyAgent`

---

## How the model was trained

Three stages, in this order.

### Stage 1 — Behavioural cloning (policy distillation)

Supervised learning, not RL. The network is trained with cross-entropy to
predict **which candidate the heuristic would pick**, under teacher forcing so
the states it learns on are the ones a competent player actually reaches.

Why: PPO starting from a uniform policy over 16 candidates has to find the good
ones by trial, and the only unambiguous reward arrives once per ~72 steps at the
end of a game. Starting from a policy that already plays about as well as the
heuristic means PPO spends its samples on *improving* that policy.

The ceiling of this stage is the heuristic. Everything above it comes from PPO.

### Stage 2 — PPO (Proximal Policy Optimization)

Written out in `train.py`, not imported from an RL library:

- clipped surrogate objective (clip 0.2)
- GAE(λ = 0.95), γ = 0.997
- entropy bonus, Huber value loss
- a **curriculum** that starts on 10–20 boards and widens to the full 10–50 range
- **potential-based reward shaping** that decays to zero, so the final policy
  optimises the real win condition and not a proxy

### Stage 3 — Self-play (optional, `--preset selfplay`)

Snapshots of the learner join the opponent pool, so the policy has to keep
beating what it used to be. Two safeguards:

- **A snapshot only enters the pool if an evaluation promoted it.** Without this
  a policy that is getting worse keeps feeding weaker sparring partners into the
  pool and then trains against them — the collapse reinforces itself. This fired
  for real: our run peaked at step 501k and degraded afterwards, and the gate
  stopped the later checkpoints from poisoning the pool.
- **The heuristic keeps 40% of the pool.** Pure self-play drifts: both sides can
  agree on a bad plan and neither is punished for it.

`--preset default` has self-play switched off and is unchanged from the runs
measured before it existed.

---

## Main commands

Only the ones you actually need.

**Train without self-play:**

```bash
python train.py --workers 6 --parallel-envs 12 --steps-per-env 128 --distill-iterations 150 --total-steps 2000000 --output runs/default
```

**Train with self-play:**

```bash
python train.py --preset selfplay --workers 6 --parallel-envs 12 --steps-per-env 128 --distill-iterations 150 --total-steps 1300000 --output runs/selfplay
```

Self-play is roughly **2× slower per step**, because the opponent is a network
that has to build its own candidate list. Budget fewer steps for the same wall
clock.

**Continue training from a checkpoint:**

```bash
python train.py --resume --output runs/selfplay
```

`--resume` needs `checkpoint.pt` (weights + optimiser + step count) inside
`--output`. To start from a `best.pt`, which holds weights only:

```bash
python train.py --init-from best.pt --output runs/next
```

**Find the fastest worker count for your machine** (run it on an idle machine;
more workers is *not* automatically faster):

```bash
python tools/bench_workers.py
```

**Evaluate a checkpoint on held-out maps:**

```bash
python tools/evaluate_model.py --checkpoint best.pt --split test --maps 60
```

**Watch a game, move by move:**

```bash
python tools/record_game.py --checkpoint best.pt --map bigtest.xml --out game.json
```

### Reading the training log

```json
{"step": 128, "greedy_churn": 0.042, "episodes_finished": 2, "mean_terminal_reward": -1.0, "policy_loss": -0.0045, "value_loss": 0.1393, "entropy": 1.581}
```

- **`greedy_churn`** is the health check: the fraction of decisions on a fixed
  probe set whose argmax changed. Entropy is blind to this — a configuration
  that changed 47% of its chosen moves moved entropy by 0.03. If churn sits at
  exactly 0 for many updates in a row, the policy has stopped learning.
- **`mean_terminal_reward`** is noise at this sample size (0–6 games per update
  against a mixed opponent pool). Do not read a trend into it.
- The lines that matter are the `eval` lines, printed every 100k steps.

`best.pt` is selected on **normalised margin** (per-game creature lead divided
by creatures on the board), not raw margin. Raw per-game margins reach ±294 on a
wipe-out, so a single blow-out moves a 120-game mean by a quarter of its whole
confidence interval — selecting on that is largely selecting which evaluation
drew the lucky game.

---

## Tournament day

### What you need

| | |
|---|---|
| The whole `RL_Model/` folder | the weights are useless without the code that builds the candidate turns |
| `best.pt` **beside `main.py`** | this is how the client finds it |
| Python 3.11+ | |
| torch, numpy | **only for the model.** Without them the heuristic plays. |

The professor's launcher appends only the ip and the port:

```bash
python main.py 123.123.3.5 6666
```

There is **no `--checkpoint` flag in that command**, so the client looks for
`best.pt` next to `main.py` on its own. Drop the file in to turn the model on,
take it out (or pass `--no-policy`) to play the heuristic.

### Before the day, on the machine that will play

```bash
python tools/package_submission.py --checkpoint best.pt --out submission
```

This copies the folder, starts a mock server that speaks the protocol from the
assignment PDF, launches `main.py` as a separate process exactly as the launcher
would, and reports whether a legal MOV came back in time. Expected output:

```
all 17 runtime modules present
checkpoint copied in as best.pt (314 KB)
  turn 1:     16 ms  1 move(s)
  OK   4 legal turns, slowest 16 ms of 2000 ms
READY
```

The one thing to check on the real machine is whether **torch is installed**.
If it is not, the client still plays — just with the heuristic.

### What happens when something breaks

Four layers, in order: **policy → heuristic → naive → single legal move.**

- Checkpoint missing, corrupt, or torch absent → heuristic plays.
- The policy raises mid-game → caught, heuristic plays.
- The policy proposes an illegal turn → rejected by `validate_turn`, heuristic
  plays.
- Under 0.5 s left → the policy is skipped entirely.

Nothing in this chain can stop the client answering. A client that cannot
communicate caps the grade at 10/20, so the model is an upgrade on the
heuristic, never a dependency of it.

### Timing

The server allows 2 s per turn. Measured per decision:

| Map | Size | Policy | Heuristic |
|---|---|---|---|
| testmap2 | 15×29 | 11 ms | 1 ms |
| bigtest | 50×40 | 28 ms | 3 ms |
| large 50×50 | 50×50 | 8 ms | 1 ms |

Worst observed: 61 ms. The client budgets 1.5 s and warms the network up before
opening the socket, because the first forward pass costs ~115 ms.

---

## Results, honestly

120 games on 60 held-out test maps, paired on the same maps, both species:

| Policy | W | L | D | Normalised margin | Outcome |
|---|---|---|---|---|---|
| heuristic vs itself | 12 | 11 | 97 | +0.002 | +0.008 |
| no self-play | 48 | 68 | 4 | −0.064 | **−0.167** |
| **self-play** | 27 | 20 | 73 | **+0.042** | **+0.058** |

Paired difference against the heuristic:

```
no-selfplay   -0.066 +/- 0.166   indistinguishable
selfplay      +0.040 +/- 0.104   indistinguishable
```

**The self-play checkpoint is clearly the better of the two models** — a winning
record where the other has a losing one. Against the heuristic it is ahead on
every column, but the confidence interval still touches zero, so it is **at
parity with the heuristic, not proven better**.

That is the honest state of it. A 78k-parameter network reaching the heuristic's
level across every board size from 10×10 to 50×50, with one set of weights, is a
real result; beating it is not something we have evidence for.

One measurement worth keeping in mind: our first evaluation set was 20 maps, and
it was unrepresentative — the heuristic playing itself drew 82.5% of games on
those maps against 29% on the test split, and only 7 of 40 games had a non-zero
margin. A checkpoint measured **+3.1 on that set and −5.5 on the test split**.
`evaluation_maps` is now 60 for that reason.

---

## Layout

| File | Holds |
|---|---|
| `main.py` | Tournament client: arguments, game loop, END/BYE, per-turn deadline, the fallback chain |
| `train.py` | Distillation, PPO, self-play snapshots, evaluation, checkpointing |
| `config.py` | Every tunable, and the `default` / `selfplay` / `smoke` presets |
| `vvw/protocol.py` | Binary framing: exact-length reads, decoding, MOV serialisation |
| `vvw/state.py` | Sparse board, populations, groups, eight-way neighbours |
| `vvw/rules.py` | Combat maths and move legality; `Unknowns` holds every unsettled rule |
| `vvw/simulator.py` | Applying a turn, terminal detection, winner |
| `vvw/evaluate.py` | Scoring a board: population, house reachability, threat, waste |
| `vvw/candidates.py` | Proposing the legal turns a policy chooses between |
| `vvw/features.py` | Board to network planes, plus per-candidate features |
| `vvw/network.py` | The fully convolutional policy-value network |
| `vvw/env.py` | Gymnasium environment used for training only |
| `vvw/vecenv.py` | Multiprocess environment pool |
| `vvw/mapgen.py` | Generating fair maps, with train / validation / test seed splits |
| `vvw/maps.py` | Reading the server's XML maps |
| `vvw/agents/heuristic.py` | The baseline agent described above |
| `vvw/agents/policy.py` | The network wrapped as an agent |
| `vvw/agents/naive.py`, `random_legal.py`, `aggressive.py` | Sparring partners and fallbacks |
| `tools/` | Benchmarking, evaluation, match series, game recording, packaging |
| `tests/` | 331 tests, including a full game over a real socket |

---

## A note on the course rules

The assignment forbids libraries that implement AI, trees or graphs. Deep
learning libraries were explicitly allowed by the professor.

- The PPO implementation — rollout collection, GAE, the clipped objective, the
  update loop — is written out in `train.py`, not imported.
- The heuristic's search is our own beam assembly.
- No tree or graph library is used anywhere; distance is Chebyshev arithmetic.
- torch and numpy are used for the neural network only. Gymnasium is used for
  the training environment and is **not** needed to play.
