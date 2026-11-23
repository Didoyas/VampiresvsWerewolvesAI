// twilight.h: the shared types and the functions each source file provides.
// Every .cpp file talks to the others only through this header.
#pragma once
#include <cstdint>
#include <string>
#include <vector>

// ---------- Shared data types ----------
enum class Species : uint8_t { None, Human, Vampire, Werewolf };

inline Species enemyOf(Species s) {
    return s == Species::Vampire ? Species::Werewolf : Species::Vampire;
}

struct Group {             // one non-empty cell
    int x, y;              // x = column, y = row, (0,0) = top-left
    int count;
    Species species;
};

struct Move {              // sent to the server as 5 bytes
    int fromX, fromY, count, toX, toY;
};
using Turn = std::vector<Move>;   // one turn = one or more moves

struct State {
    int rows = 0, cols = 0;
    std::vector<Group> groups;    // every non-empty cell on the board
};

// ---------- Networking (src/client.cpp): works on Windows and macOS ----------
struct Message {
    std::string code;              // "SET", "HUM", "HME", "MAP", "UPD", "END", "BYE"
    std::vector<int> header;       // SET: {rows, cols}   HME: {x, y}
    std::vector<Group> cells;      // MAP / UPD cells
};

class Client {
public:
    bool connectTo(const std::string& ip, int port);
    void sendName(const std::string& name);    // NME
    Message receive();                         // blocks until a full message arrives
    void sendTurn(const Turn& t);              // MOV
private:
    long long sock_ = -1;   // int on Mac, SOCKET on Windows — cast inside client.cpp
};

// ---------- Game rules (src/rules.cpp) ----------
// Build the board from the MAP message, then apply each UPD on top.
void applyMap(State& s, const std::vector<Group>& cells);
void applyUpdate(State& s, const std::vector<Group>& changedCells);

// Return the board after `who` plays `t`, resolving conversions and battles
// (use expected values for random battles).
State applyTurn(const State& s, const Turn& t, Species who);

int  totalCount(const State& s, Species who);
bool isTerminal(const State& s);  // one monster species has 0 creatures

// ---------- Evaluation (src/eval.cpp) ----------
// The weights of each heuristic term. The defaults are the values found by
// self-play tuning (tools/tune.cpp). Each thread has its own copy, so the
// tuner can play two differently weighted bots against each other.
struct EvalWeights {
    double material = 10.0;   // per creature
    double ratio    = 20.0;   // (me - them) / (me + them)
    double food     = 4.0;    // per human we reach first, divided by distance
    double prey     = 3.0;    // per enemy creature we can sure-kill, divided by distance
    double danger   = 3.0;    // per own creature that can be sure-killed, divided by distance
    double strike   = 6.0;    // per creature, when a sure kill is possible right now
    double risk     = 1.0;    // per own creature next to an enemy we can't sure-beat
    double groups   = 1.5;    // per extra group (one big group is stronger)
    double centre   = 0.05;   // per creature, tiny tie-breaker
};
void setEvalWeights(const EvalWeights& w);
EvalWeights getEvalWeights();

// Score from `me`'s point of view: positive = good for me.
// ply    = how many turns below the current position (use it to prefer faster wins).
// toMove = whose turn it is in `s` (tempo: the side to move wins tied races).
double evaluate(const State& s, Species me, int ply, Species toMove);

// ---------- Move generation (src/moves.cpp) ----------
// Candidate turns for `who` (NOT every possible split — keep the list small).
// Every turn must be legal: inside the board, >= 1 creature moved,
// no cell is both a source and a target in the same turn.
// Return them best-first when possible: it makes alpha-beta prune more.
std::vector<Turn> generateTurns(const State& s, Species who);

// ---------- Search (src/search.cpp) ----------
// Alpha-beta with iterative deepening; must return before timeLimitSec.
Turn findBestTurn(const State& s, Species me, double timeLimitSec);

// Options used by the tuner: a fixed maximum depth (0 = no limit, the default)
// makes games fast and repeatable; quiet mode turns off the per-move log line.
void setSearchDepthLimit(int maxDepth);
void setSearchQuiet(bool quiet);
