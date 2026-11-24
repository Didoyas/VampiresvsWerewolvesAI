# Twilight AI

An alpha-beta bot for the Vampires vs Werewolves (Twilight) game, written in C++17.

Course project, MSc AI (CentraleSupélec / Université Paris-Saclay).

## Table of contents

- [About](#about)
- [Getting started](#getting-started)
- [Usage](#usage)
- [Running the tests](#running-the-tests)
- [Course rules](#course-rules)
- [How the bot works](#how-the-bot-works)
- [Where each algorithm is implemented](#where-each-algorithm-is-implemented)
- [Results](#results)
- [Project structure](#project-structure)
- [Acknowledgments](#acknowledgments)

## About

Twilight is a turn-based strategy game on a grid. Two players (vampires and werewolves) move their groups one cell at a time, convert humans to grow, and fight each other. Each move must be sent within 2 seconds. The game ends when one species disappears, or after a maximum time or number of plies; the species with more creatures wins.

This bot connects to the game server over TCP, keeps its own copy of the board, and picks each move with an alpha-beta search that uses iterative deepening and stops before the time limit.

## Getting started

### Prerequisites

- A C++17 compiler (Clang, GCC or MSVC)
- [CMake](https://cmake.org/) 3.15 or newer
- A game server:
  - the official server provided by the course (Windows, .NET Framework 4.5), or
  - [Succo/twilight](https://github.com/Succo/twilight), a cross-platform server written in Go

### Build

macOS / Linux:

```bash
cmake -B build
cmake --build build
```

Windows (Developer PowerShell for Visual Studio):

```powershell
cmake -B build
cmake --build build --config Release
```

The default build type is Release, because search depth depends on speed. On Windows the C++ runtime is linked statically, so the program runs on the CTI machines (where the tournament is played) without installing anything.

## Usage

Start the server, then run the bot. As the course rules require, the IP and the port are always the **last two** arguments, so the tournament launcher can add its own parameters in front:

```bash
./build/twilight 127.0.0.1 5555                 # macOS / Linux
.\build\Release\twilight.exe 127.0.0.1 5555     # Windows
```

The bot sends its name (`AlphaBeta`, set in `src/main.cpp`), then plays every game the server starts until it receives `BYE`. Each turn it prints a line like this to stderr:

```
[search] depth 12, 853248 nodes, score 10, 1.70045 s
```

## Running the tests

```bash
ctest --test-dir build --output-on-failure        # all tests
```

| Test program | What it checks |
|---|---|
| `twilight_rules_tests` | the game rules case by case (conversions, the 1.5× rule both ways, random battles, splits, merges, MAP and UPD updates) and basic properties of the evaluation |
| `twilight_tests` | bot-vs-bot games starting from the two course maps (`thetrap`, `testmap2`), a 50×50 board, a 255×255 board with counts up to 255, and a crowded 10×10 board: every move legal and on time, plus the fallback move on a 5×5 board |
| `twilight_protocol_tests` | a fake server starts the real `twilight` program the way the tournament launcher does, and plays two full games against it (one per species, with `END` in between, then `BYE`): every message well formed, every move legal, every answer within 2 s (macOS / Linux only) |

## Course rules

How the code follows the rules in the assignment:

| Rule | Where it is handled |
|---|---|
| Rules 1 and 6: at least one move, at least one creature | `generateTurns()` never returns an empty turn; `fallbackTurn()` as a last resort |
| Rule 2: only move our own groups | options are built only from our groups |
| Rule 3: enough creatures on a cell for all its moves | a split's two parts add up to the group size |
| Rule 4: 8 directions, not off the board | every move is one step; off-board cells are skipped |
| Rule 5: no cell both source and target in one ply | `legal()` in `moves.cpp` |
| Convert humans: at least as many as them (confirmed on the professor's server) | `resolveArrival()` in `rules.cpp`, `arrivalValue()` in `moves.cpp`, `eatDistance()` in `eval.cpp` |
| Kill monsters: at least 1.5×; if they are 1.5× us, all attackers die | `resolveArrival()` in `rules.cpp` |
| Random battles: P = 0.5, E1/(2·E2) or E1/E2 − 0.5 | `winProbability()` in `rules.cpp` |
| Protocol: 3-letter commands, every number one byte | `Client::receive()`, `Client::sendTurn()` in `client.cpp` |
| 2 seconds per turn | search stops at 1.2 s (0.8 s margin for the network and a slow machine); the clock is checked every 16 positions |
| `END`: clear the game and prepare a new one | game loop in `main.cpp` |
| Grid sizes and counts are one byte (0–255) | stored as normal integers; tests cover 255×255 and counts up to 255 |
| IP and port are the last two command-line arguments | `main()` in `main.cpp` |
| Take the opponent's moves into account (not a reflex agent) | alpha-beta searches the opponent's replies |
| No libraries implementing AI, trees or graphs | everything is written from scratch; only the C++ standard library and the system's socket library are used |

## How the bot works

Every turn, the bot does three things:

1. **Reads the board.** `client.cpp` decodes the server's messages; `rules.cpp` builds the board from `MAP` and applies each `UPD`.
2. **Searches.** `search.cpp` runs alpha-beta over the candidate turns from `moves.cpp`, simulating each turn with `rules.cpp` and scoring the resulting boards with `eval.cpp`.
3. **Answers.** `client.cpp` sends the chosen turn as `MOV`.

### Networking (`client.cpp`)

POSIX sockets on macOS / Linux and Winsock on Windows. Reads always loop until every expected byte has arrived, and Nagle's algorithm is turned off so each `MOV` leaves immediately. Each `MAP` / `UPD` cell is turned into a group; a cell with all counts at 0 means it is now empty.

### Game rules (`rules.cpp`)

`applyTurn()` moves the creatures, merges groups of the same species, and resolves conversions and battles. The search needs one board per move, so every random battle is resolved pessimistically for the attacker, with the expected number of survivors: the bot never counts on luck.

Against humans, at least as many creatures converts them for sure, as in the PDF; tests on the professor's server confirmed that an attack with exactly as many creatures as humans always converts them. (The open-source Go server used for local testing makes that case a coin flip, so equal attacks can occasionally fail there.) Against monsters, the bot plays safe: any fight below the 1.5× needed for a sure kill is treated as a likely loss.

### Evaluation (`eval.cpp`)

Scores a board from our point of view, looping over groups only (it runs hundreds of thousands of times per move):

| Term | Idea |
|---|---|
| Win / loss | ±1,000,000, minus the depth, so faster wins score higher |
| Material | our creatures − theirs, plus a ratio term (8 vs 4 is safer than 108 vs 104) |
| Food race | each human group counts for whoever can eat it for sure (at least as many creatures) and gets there first; a tie goes to the side about to move (tempo) |
| Attack or run | enemies we can sure-kill nearby are good; enemies that can sure-kill us nearby are bad, much more so when the attacker moves next |
| Game phase | once all humans are gone, fights count double |
| Extras | fewer, bigger groups; a tiny preference for the centre |

### Move generation (`moves.cpp`)

Once splitting is allowed, there are far too many possible turns to search them all. So for each group, `generateTurns()` builds a small set of options:

- stay in place
- move the whole group one step (8 directions)
- split into two parts, each heading toward a different human group that we can reach before the enemy

Each option gets a fast, battle-aware score:

| Situation | Effect |
|---|---|
| Eating humans (group ≥ humans) | bonus |
| Sure kill (group ≥ 1.5 × enemy) | bonus |
| Random battle | penalty |
| Walking into a sure loss | big penalty |
| Ending next to an enemy that can beat us | penalty |
| Getting closer to safe food or prey | bonus |
| Getting closer to a stronger enemy | penalty |
| Splitting | small cost |
| Distance from the centre | tiny penalty (tie-breaker) |

The options are combined into at most 20 legal turns, sorted best first, so that alpha-beta prunes more and searches deeper. `ALLOW_SPLITS` at the top of the file turns splitting off, in case the official server rejects a split (the rules say a `MOV` holds "0 ≤ n ≤ number of groups" moves).

### Search (`search.cpp`)

- **Alpha-beta (negamax form):** one function handles both players.
- **Iterative deepening:** searches depth 1, 2, 3, … and keeps the best move from the last finished depth, so a move is always ready.
- **Time control:** stops at the given limit (1.2 s), and doesn't start a new depth once half the time is used.
- **Transposition table:** each position is hashed in a way that doesn't depend on the order of the groups, so the same position reached by different moves isn't searched twice.
- **Move ordering:** the previous best move is searched first at the root, and the remembered best move first at every other position.
- **Always answers:** if the search fails for any reason, `findBestTurn()` falls back to a simple legal one-step move. An AI that can't communicate caps the grade at 10/20.

### Weight tuning (`tools/tune.cpp`)

The evaluation weights can be tuned automatically by self-play: the tuner changes one weight at a time (× 2.5 or ÷ 2.5), plays the changed bot against the current one on 50 random point-symmetric maps, twice per map with the sides swapped, and keeps the change only if it clearly wins (score ≥ 0.56). Games are refereed with real random battles.

```bash
./build/twilight_tune 3 50 6 2          # rounds, maps per test, search depth, threads
./build/twilight_tune try 100 6 2 danger=1.2 groups=3    # test one set of weights
```

Result of the last run (about 30 minutes, 2,000+ games): no change won clearly, so the hand-picked weights in `twilight.h` were kept. Lowering the food weight clearly made the bot worse (10 wins, 22 losses), confirming that racing for humans matters; the two slightly promising changes (lower danger, more weight on staying in fewer groups) did not hold up together on 100 fresh maps (29 wins, 33 losses).

## Where each algorithm is implemented

| Algorithm | File | Function |
|---|---|---|
| Alpha-beta search (negamax form) | `src/search.cpp` | `Searcher::negamax()` |
| Iterative deepening and time control | `src/search.cpp` | `searchBestTurn()` |
| Self-play weight tuning | `tools/tune.cpp` | `match()`, `playGame()`, `main()` |
| Transposition table and position hashing | `src/search.cpp` | `Searcher::negamax()`, `hashState()` |
| Fallback move | `src/search.cpp` | `fallbackTurn()`, called by `findBestTurn()` |
| Candidate move generation | `src/moves.cpp` | `generateTurns()` |
| Per-group options (stay, step, split) | `src/moves.cpp` | `optionsForGroup()` |
| Move-ordering score | `src/moves.cpp` | `moveScore()`, `arrivalValue()`, `positionValue()` |
| Turn legality (rule 5) | `src/moves.cpp` | `legal()` |
| Evaluation function | `src/eval.cpp` | `evaluate()` |
| Turn simulation and battles | `src/rules.cpp` | `applyTurn()`, `resolveArrival()`, `winProbability()` |
| Board updates from the server | `src/rules.cpp` | `applyMap()`, `applyUpdate()` |
| Protocol and TCP connection | `src/client.cpp` | `Client::connectTo()`, `receive()`, `sendName()`, `sendTurn()` |
| Game loop | `src/main.cpp` | `main()` |

## Results

From the tests above, with a 1.7 s limit (times vary a little between runs):

| Board | Size | Slowest move |
|---|---|---|
| thetrap | 10×5 | 1.2 s |
| random, 40 houses | 50×50 | 1.6 s |
| random, counts up to 255 | 255×255 | 1.1 s |
| crowded, 90 houses | 10×10 | 1.6 s |
| full games over a socket (fake server) | 10×5 | 1.4 s per answer |

The search reaches depth 12–16 on thetrap and 6–8 on the 50×50 board. In the protocol test (8 turns per game), the bot (1.7 s per move) was ahead of the fake server's opponent (0.1 s per move) at the end of both games.

## Project structure

```
.
├── include/
│   └── twilight.h            # shared types and function signatures
├── src/
│   ├── main.cpp              # game loop
│   ├── client.cpp            # TCP client and protocol
│   ├── rules.cpp             # board updates and game rules
│   ├── eval.cpp              # evaluation function
│   ├── moves.cpp             # candidate move generation
│   └── search.cpp            # alpha-beta search
├── tests/
│   ├── test_rules.cpp        # rules and evaluation, case by case
│   ├── test_search.cpp       # bot-vs-bot legality and timing tests
│   └── test_protocol.cpp     # fake server vs the real program
├── tools/
│   └── tune.cpp              # self-play weight tuning
├── CMakeLists.txt
├── .gitattributes            # same line endings on Windows and macOS
├── .gitignore
└── README.md
```

## Acknowledgments

- Game, rules and official server: provided by the course
- [Succo/twilight](https://github.com/Succo/twilight), an open-source Go server used for local testing
