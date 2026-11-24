// tune.cpp: tunes the evaluation weights by self-play.
//
// How it works:
//   1. Start from the current weights (the defaults in twilight.h).
//   2. Make a candidate by changing ONE weight (x step or / step; step = 2.5 by default).
//   3. Play the candidate against the current weights on many random maps,
//      twice per map with the sides swapped, so neither side gets an advantage
//      from the map or from moving first.
//   4. If the candidate wins clearly, it becomes the new current weights.
//   5. Repeat for every weight, for a few rounds.
//
// Games are refereed with real random battles (equal numbers convert humans, as on
// the professor's server), while each bot keeps its own careful model of the rules.
// Maps are point-symmetric, so both sides start in mirrored positions.
//
// Usage: twilight_tune [rounds] [maps] [depth] [threads] [step]
//        twilight_tune validate [maps] [depth] [threads]   (tuned vs old defaults)
//        twilight_tune try maps depth threads name=value ...  (one candidate vs the defaults)
#include "twilight.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <iostream>
#include <map>
#include <random>
#include <set>
#include <string>
#include <thread>
#include <vector>

namespace {

// ---------- Referee: applies a turn like the Go server does ----------

double serverProbability(int e1, int e2, bool humans) {
    if (humans && e1 >= e2) return 1.0;             // professor's server: equal converts
    if (e1 == e2) return 0.5;
    double cste = humans ? 1.0 : 1.5;
    if (e1 >= cste * e2) return 1.0;
    if (e1 < e2) return 0.5 * e1 / e2;
    return 0.5;                                     // the server's formula gives 0.5 here
}

State refereeTurn(const State& s, const Turn& t, Species who, std::mt19937& rng) {
    std::uniform_real_distribution<double> U(0.0, 1.0);
    std::map<std::pair<int, int>, Group> cells;
    for (const Group& g : s.groups) cells[{g.x, g.y}] = g;
    std::map<std::pair<int, int>, int> arriving;
    for (const Move& m : t) {
        cells[{m.fromX, m.fromY}].count -= m.count;
        arriving[{m.toX, m.toY}] += m.count;
    }
    for (auto& [pos, n] : arriving) {
        auto it = cells.find(pos);
        if (it == cells.end() || it->second.count <= 0) { cells[pos] = {pos.first, pos.second, n, who}; continue; }
        Group& d = it->second;
        if (d.species == who) { d.count += n; continue; }
        bool humans = d.species == Species::Human;
        double p = serverProbability(n, d.count, humans);
        if (U(rng) < p) {                            // attackers win
            int pool = humans ? n + d.count : n, survivors = 0;
            for (int i = 0; i < pool; ++i) if (U(rng) < p) ++survivors;
            d.count = survivors; d.species = who;
        } else {                                     // defenders win
            int survivors = 0;
            for (int i = 0; i < d.count; ++i) if (U(rng) < 1 - p) ++survivors;
            d.count = survivors;
        }
    }
    State out; out.rows = s.rows; out.cols = s.cols;
    for (auto& [pos, g] : cells) if (g.count > 0) out.groups.push_back(g);
    return out;
}

bool legal(const State& s, const Turn& t, Species who) {
    if (t.empty()) return false;
    std::set<std::pair<int, int>> sources;
    std::map<std::pair<int, int>, int> used;
    for (const Move& m : t) sources.insert({m.fromX, m.fromY});
    for (const Move& m : t) {
        if (m.count < 1 || m.toX < 0 || m.toY < 0 || m.toX >= s.cols || m.toY >= s.rows) return false;
        if (std::max(std::abs(m.toX - m.fromX), std::abs(m.toY - m.fromY)) != 1) return false;
        if (sources.count({m.toX, m.toY})) return false;
        used[{m.fromX, m.fromY}] += m.count;
    }
    for (auto& [p, n] : used) {
        bool ok = false;
        for (const Group& g : s.groups) if (g.x == p.first && g.y == p.second && g.species == who && g.count >= n) ok = true;
        if (!ok) return false;
    }
    return true;
}

// ---------- Fair random maps (point-symmetric) ----------

State randomMap(unsigned seed) {
    std::mt19937 rng(seed);
    auto R = [&](int lo, int hi) { return std::uniform_int_distribution<int>(lo, hi)(rng); };
    State s; s.rows = R(6, 20); s.cols = R(8, 25);
    std::set<std::pair<int, int>> used;
    auto mirror = [&](int x, int y) { return std::make_pair(s.cols - 1 - x, s.rows - 1 - y); };
    auto place = [&](Species sp, int count, bool monsters) {
        for (int tries = 0; tries < 200; ++tries) {
            int x = R(0, s.cols - 1), y = R(0, s.rows - 1);
            auto m = mirror(x, y);
            if (m == std::make_pair(x, y) || used.count({x, y}) || used.count(m)) continue;
            used.insert({x, y}); used.insert(m);
            if (monsters) {
                s.groups.push_back({x, y, count, Species::Vampire});
                s.groups.push_back({m.first, m.second, count, Species::Werewolf});
            } else {
                s.groups.push_back({x, y, count, sp});
                s.groups.push_back({m.first, m.second, count, sp});
            }
            return;
        }
    };
    int monsters = R(4, 15);
    place(Species::Vampire, monsters, true);
    int pairs = R(2, std::max(3, s.rows * s.cols / 25));
    for (int i = 0; i < pairs; ++i) place(Species::Human, R(1, monsters + 4), false);
    return s;
}

// ---------- Playing games ----------

struct Result { double score = 0; int wins = 0, losses = 0, draws = 0; double margin = 0; };

// One game: `a` plays `aSide`; vampires move first. Returns 1 / 0.5 / 0 for `a`.
double playGame(const State& start, const EvalWeights& a, const EvalWeights& b, Species aSide,
                unsigned seed, int maxPlies, double* margin) {
    std::mt19937 rng(seed);
    State s = start;
    Species toMove = Species::Vampire;
    for (int ply = 0; ply < maxPlies && !isTerminal(s); ++ply) {
        setEvalWeights(toMove == aSide ? a : b);
        Turn t = findBestTurn(s, toMove, 5.0);
        if (!legal(s, t, toMove)) {                  // an illegal turn loses the game
            *margin = toMove == aSide ? -1 : 1;
            return toMove == aSide ? 0.0 : 1.0;
        }
        s = refereeTurn(s, t, toMove, rng);
        toMove = enemyOf(toMove);
    }
    int mine = totalCount(s, aSide), theirs = totalCount(s, enemyOf(aSide));
    *margin = (mine + theirs) ? double(mine - theirs) / (mine + theirs) : 0;
    return mine > theirs ? 1.0 : mine < theirs ? 0.0 : 0.5;
}

// Candidate vs base on `maps` maps, each played twice with the sides swapped.
Result match(const EvalWeights& cand, const EvalWeights& base, unsigned firstSeed, int maps,
             int depth, int threads, int maxPlies) {
    std::vector<double> scores(2 * maps), margins(2 * maps);
    std::atomic<int> next{0};
    auto worker = [&] {
        setSearchDepthLimit(depth);
        setSearchQuiet(true);
        for (int i; (i = next++) < 2 * maps;) {
            unsigned seed = firstSeed + unsigned(i / 2);
            State m = randomMap(seed);
            Species side = (i % 2 == 0) ? Species::Vampire : Species::Werewolf;
            scores[i] = playGame(m, cand, base, side, seed * 7919u + 1, maxPlies, &margins[i]);
        }
    };
    std::vector<std::thread> pool;
    for (int k = 0; k < threads; ++k) pool.emplace_back(worker);
    for (auto& th : pool) th.join();
    Result r;
    for (int i = 0; i < 2 * maps; ++i) {
        r.score += scores[i]; r.margin += margins[i];
        if (scores[i] == 1) ++r.wins; else if (scores[i] == 0) ++r.losses; else ++r.draws;
    }
    r.score /= 2 * maps; r.margin /= 2 * maps;
    return r;
}

// The hand-picked weights the bot started with, kept to measure the tuned ones against.
EvalWeights OLD_DEFAULTS() {
    EvalWeights w;
    w.material = 10; w.ratio = 20; w.food = 4; w.prey = 3; w.danger = 3;
    w.strike = 6; w.risk = 1; w.groups = 1.5; w.centre = 0.05;
    return w;
}

double* field(EvalWeights& w, int k) {
    double* f[] = {&w.material, &w.ratio, &w.food, &w.prey, &w.danger, &w.strike, &w.risk, &w.groups, &w.centre};
    return f[k];
}
const char* NAMES[] = {"material", "ratio", "food", "prey", "danger", "strike", "risk", "groups", "centre"};

void print(const EvalWeights& w) {
    EvalWeights c = w;
    for (int k = 0; k < 9; ++k) std::printf("    double %-8s = %g;\n", NAMES[k], *field(c, k));
}

}  // namespace

int main(int argc, char** argv) {
    const int maxPlies = 100;                         // the Go server plays 50 rounds
    if (argc > 1 && std::string(argv[1]) == "try") {
        // twilight_tune try maps depth threads name=value ...  (candidate vs current defaults)
        int maps = std::atoi(argv[2]), depth = std::atoi(argv[3]), threads = std::atoi(argv[4]);
        EvalWeights cand;
        for (int i = 5; i < argc; ++i) {
            std::string kv = argv[i]; auto eq = kv.find('=');
            for (int k = 0; k < 9; ++k)
                if (kv.substr(0, eq) == NAMES[k]) *field(cand, k) = std::atof(kv.c_str() + eq + 1);
        }
        Result r = match(cand, EvalWeights{}, 200000, maps, depth, threads, maxPlies);
        std::printf("candidate vs current on %d fresh maps (x2 sides): %d wins, %d losses, %d draws, "
                    "score %.3f, average margin %+.3f\n", maps, r.wins, r.losses, r.draws, r.score, r.margin);
        return 0;
    }
    if (argc > 1 && std::string(argv[1]) == "validate") {
        int maps = argc > 2 ? std::atoi(argv[2]) : 40, depth = argc > 3 ? std::atoi(argv[3]) : 4;
        int threads = argc > 4 ? std::atoi(argv[4]) : 2;
        Result r = match(EvalWeights{}, OLD_DEFAULTS(), 100000, maps, depth, threads, maxPlies);
        std::printf("tuned vs old weights on %d fresh maps (x2 sides): %d wins, %d losses, %d draws, "
                    "score %.3f, average margin %+.3f\n", maps, r.wins, r.losses, r.draws, r.score, r.margin);
        return 0;
    }
    int rounds = argc > 1 ? std::atoi(argv[1]) : 2, maps = argc > 2 ? std::atoi(argv[2]) : 24;
    int depth = argc > 3 ? std::atoi(argv[3]) : 4, threads = argc > 4 ? std::atoi(argv[4]) : 2;
    double step = argc > 5 ? std::atof(argv[5]) : 2.5;   // each test multiplies one weight by step or 1/step

    EvalWeights best = getEvalWeights();
    std::printf("Tuning: %d rounds, %d maps x 2 sides per test, depth %d, %d threads\n", rounds, maps, depth, threads);
    unsigned seed = 1;
    for (int round = 1; round <= rounds; ++round) {
        bool improved = false;
        for (int k = 0; k < 9; ++k)
            for (double f : {step, 1.0 / step}) {
                EvalWeights cand = best;
                *field(cand, k) *= f;
                auto t0 = std::chrono::steady_clock::now();
                Result r = match(cand, best, seed, maps, depth, threads, maxPlies);
                double sec = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
                bool accept = r.score >= 0.56 && r.wins > r.losses;
                std::printf("round %d  %-8s x%-5.2f  %2dW %2dL %2dD  score %.3f  margin %+.3f  (%.0fs)%s\n",
                            round, NAMES[k], f, r.wins, r.losses, r.draws, r.score, r.margin, sec,
                            accept ? "  <- accepted" : "");
                std::fflush(stdout);
                if (accept) { best = cand; improved = true; break; }
            }
        seed += unsigned(maps);                       // fresh maps for the next round
        if (!improved) { std::printf("No change in round %d: stopping.\n", round); break; }
    }
    std::printf("\nBest weights:\n");
    print(best);
    return 0;
}
