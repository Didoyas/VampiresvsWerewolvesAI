// search.cpp: alpha-beta (negamax form) + iterative deepening
// + time limit + transposition table + move ordering.
//
// Negamax = minimax written once: "my score = minus the opponent's best score".
// Iterative deepening = search depth 1, 2, 3, ... and keep the last finished
// answer, so we always have a move ready when the clock runs out.
#include "twilight.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <iostream>
#include <limits>
#include <numeric>
#include <stdexcept>
#include <unordered_map>

namespace {

using Clock = std::chrono::steady_clock;
constexpr double INF       = std::numeric_limits<double>::infinity();
constexpr int    MAX_DEPTH = 30;
constexpr double WIN_SCORE = 5e5;      // |score| above this = forced win/loss found
constexpr size_t TT_LIMIT  = 2'000'000;

struct Timeout {};                     // thrown when time is up, caught at the root

thread_local int  depthLimit = 0;      // 0 = no limit (normal play)
thread_local bool quietLog   = false;

// ---------- Hashing a position (order of groups does not matter) ----------
uint64_t mix(uint64_t x) {             // splitmix64: turns a number into a random-looking one
    x += 0x9e3779b97f4a7c15ULL;
    x = (x ^ (x >> 30)) * 0xbf58476d1ce4e5b9ULL;
    x = (x ^ (x >> 27)) * 0x94d049bb133111ebULL;
    return x ^ (x >> 31);
}
uint64_t hashState(const State& s, Species toMove) {
    uint64_t h = toMove == Species::Vampire ? 0x1234567ULL : 0x7654321ULL;
    for (const Group& g : s.groups) {
        if (g.count <= 0) continue;
        uint64_t k = (uint64_t(g.x) << 40) | (uint64_t(g.y) << 24) |
                     (uint64_t(g.species) << 16) | uint64_t(g.count);
        h ^= mix(k);                   // XOR = same hash whatever the group order
    }
    return h;
}

enum Flag : uint8_t { EXACT, LOWER, UPPER };
struct TTEntry { int depth; double value; Flag flag; int best; };

struct Searcher {
    Species me;
    Clock::time_point deadline;
    long long nodes = 0;
    std::unordered_map<uint64_t, TTEntry> tt;

    void checkTime() {
        if ((++nodes & 15) == 0 && Clock::now() >= deadline) throw Timeout{};   // every 16 positions: crowded boards make each position slow
    }

    // Score of `s` from the point of view of `toMove` (negamax convention).
    double leaf(const State& s, int ply, Species toMove) {
        double e = evaluate(s, me, ply, toMove);   // from `me`'s point of view
        return toMove == me ? e : -e;
    }

    double negamax(const State& s, int depth, int ply, double alpha, double beta, Species toMove) {
        checkTime();
        if (depth == 0 || isTerminal(s)) return leaf(s, ply, toMove);

        // Transposition table: reuse work if we have seen this position before.
        uint64_t h = hashState(s, toMove);
        int ttBest = -1;
        auto it = tt.find(h);
        if (it != tt.end()) {
            const TTEntry& e = it->second;
            ttBest = e.best;
            if (e.depth >= depth) {
                if (e.flag == EXACT) return e.value;
                if (e.flag == LOWER && e.value >= beta) return e.value;
                if (e.flag == UPPER && e.value <= alpha) return e.value;
            }
        }

        std::vector<Turn> turns = generateTurns(s, toMove);
        if (turns.empty()) return leaf(s, ply, toMove);

        // Move ordering: TT best move first, then generator order (already best-first).
        std::vector<int> order(turns.size());
        std::iota(order.begin(), order.end(), 0);
        if (ttBest > 0 && ttBest < int(turns.size())) {
            order.erase(order.begin() + ttBest);
            order.insert(order.begin(), ttBest);
        }

        double alpha0 = alpha, best = -INF;
        int bestIdx = order[0];
        for (int idx : order) {
            State next = applyTurn(s, turns[idx], toMove);
            double v = -negamax(next, depth - 1, ply + 1, -beta, -alpha, enemyOf(toMove));
            if (v > best) { best = v; bestIdx = idx; }
            if (v > alpha) alpha = v;
            if (alpha >= beta) break;               // cut-off: opponent won't allow this line
        }

        if (tt.size() > TT_LIMIT) tt.clear();
        Flag f = best <= alpha0 ? UPPER : (best >= beta ? LOWER : EXACT);
        tt[h] = TTEntry{depth, best, f, bestIdx};
        return best;
    }
};

// Last line of defence: one whole group, one step, to the first cell on the board.
// If no answer arrives in 2 s the server skips our turn, and an AI that can't
// communicate caps the grade at 10/20, so we always send something legal.
Turn fallbackTurn(const State& s, Species me) {
    for (const Group& g : s.groups) {
        if (g.species != me || g.count <= 0) continue;
        for (int dx = -1; dx <= 1; ++dx)
            for (int dy = -1; dy <= 1; ++dy) {
                int x = g.x + dx, y = g.y + dy;
                if ((dx || dy) && x >= 0 && y >= 0 && x < s.cols && y < s.rows)
                    return {Move{g.x, g.y, g.count, x, y}};
            }
    }
    return {};   // we have no creatures left: nothing legal exists
}

Turn searchBestTurn(const State& s, Species me, double timeLimitSec) {
    auto start = Clock::now();
    auto elapsed = [&] { return std::chrono::duration<double>(Clock::now() - start).count(); };

    std::vector<Turn> turns = generateTurns(s, me);
    if (turns.empty()) return {};
    if (turns.size() == 1) return turns[0];

    Searcher sr;
    sr.me = me;
    sr.deadline = start + std::chrono::duration_cast<Clock::duration>(
                              std::chrono::duration<double>(timeLimitSec));

    std::vector<int> order(turns.size());
    std::iota(order.begin(), order.end(), 0);
    Turn best = turns[0];                 // generator's favourite: our fallback
    int reached = 0;
    double bestScore = 0;

    int maxDepth = depthLimit > 0 ? std::min(depthLimit, MAX_DEPTH) : MAX_DEPTH;
    for (int depth = 1; depth <= maxDepth; ++depth) {
        // The next depth usually takes several times longer: don't start it
        // if we have already used half the time.
        if (depth > 1 && elapsed() > timeLimitSec * 0.5) break;
        try {
            double alpha = -INF, iterBest = -INF;
            int iterIdx = order[0];
            for (int idx : order) {
                State next = applyTurn(s, turns[idx], me);
                double v = -sr.negamax(next, depth - 1, 1, -INF, -alpha, enemyOf(me));
                if (v > iterBest) { iterBest = v; iterIdx = idx; }
                if (v > alpha) alpha = v;
            }
            best = turns[iterIdx];
            bestScore = iterBest;
            reached = depth;
            // Search last iteration's best move first next time (better pruning).
            order.erase(std::find(order.begin(), order.end(), iterIdx));
            order.insert(order.begin(), iterIdx);
            if (std::abs(iterBest) > WIN_SCORE) break;   // forced win or loss: stop
        } catch (const Timeout&) {
            break;                        // keep the result of the last finished depth
        } catch (const std::exception& e) {
            std::cerr << "[search] error at depth " << depth << ": " << e.what() << "\n";
            break;                        // same: keep the last good answer
        }
    }

    if (!quietLog)
    std::cerr << "[search] depth " << reached << ", " << sr.nodes << " nodes, score "
              << bestScore << ", " << elapsed() << " s\n";
    return best;
}

}  // namespace

// Public entry point: the search, wrapped so that it always returns a legal turn.
Turn findBestTurn(const State& s, Species me, double timeLimitSec) {
    try {
        Turn t = searchBestTurn(s, me, timeLimitSec);
        if (!t.empty()) return t;
    } catch (const std::exception& e) {
        std::cerr << "[search] failed: " << e.what() << ", using fallback move\n";
    } catch (...) {
        std::cerr << "[search] failed, using fallback move\n";
    }
    return fallbackTurn(s, me);
}

void setSearchDepthLimit(int maxDepth) { depthLimit = maxDepth; }
void setSearchQuiet(bool quiet) { quietLog = quiet; }
