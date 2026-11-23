// test_search.cpp: checks that every turn the search returns is legal and
// arrives in time, by playing bot-vs-bot games (with the real rules) on the
// course maps and on boards up to 255 x 255.
#include "twilight.h"
#include <chrono>
#include <cmath>
#include <iostream>
#include <map>
#include <set>

static std::string legalityError(const State& s, const Turn& t, Species who) {
    if (t.empty()) return "empty turn";
    std::map<std::pair<int,int>, int> used;
    std::set<std::pair<int,int>> sources;
    for (const Move& m : t) sources.insert({m.fromX, m.fromY});
    for (const Move& m : t) {
        if (m.count < 1) return "count < 1";
        if (m.count > 255) return "count > 255 (doesn't fit in one byte)";
        if (m.toX < 0 || m.toY < 0 || m.toX >= s.cols || m.toY >= s.rows) return "off board";
        if (std::max(std::abs(m.toX - m.fromX), std::abs(m.toY - m.fromY)) != 1) return "not a neighbour";
        if (sources.count({m.toX, m.toY})) return "target is also a source";
        used[{m.fromX, m.fromY}] += m.count;
    }
    for (auto& [pos, n] : used) {
        bool ok = false;
        for (const Group& g : s.groups)
            if (g.x == pos.first && g.y == pos.second && g.species == who && g.count >= n) ok = true;
        if (!ok) return "moving creatures we don't have";
    }
    return "";
}

static int playGame(const char* name, State s, Species first, double limit, int maxTurns) {
    Species toMove = first;
    double worst = 0;
    for (int turn = 0; turn < maxTurns && !isTerminal(s); ++turn) {
        auto t0 = std::chrono::steady_clock::now();
        Turn t = findBestTurn(s, toMove, limit);
        double dt = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
        worst = std::max(worst, dt);
        std::string err = legalityError(s, t, toMove);
        if (!err.empty()) { std::cout << name << ": ILLEGAL turn " << turn << ": " << err << "\n"; return 1; }
        s = applyTurn(s, t, toMove);
        toMove = enemyOf(toMove);
    }
    std::cout << name << ": vampires " << totalCount(s, Species::Vampire)
              << ", werewolves " << totalCount(s, Species::Werewolf)
              << ", humans " << totalCount(s, Species::Human)
              << " | slowest move " << worst << " s (limit " << limit << ")\n";
    return worst > limit + 0.2 ? 1 : 0;
}

int main() {
    State trap; trap.rows = 5; trap.cols = 10;
    trap.groups = {{2,2,4,Species::Human},{9,0,2,Species::Human},{9,2,1,Species::Human},
                   {9,4,2,Species::Human},{4,1,4,Species::Werewolf},{4,3,4,Species::Vampire}};
    State big; big.rows = 15; big.cols = 29;
    big.groups = {{28,0,2,Species::Human},{0,14,2,Species::Human},{14,7,20,Species::Human},
                  {10,3,4,Species::Human},{18,11,4,Species::Human},
                  {0,0,10,Species::Werewolf},{28,14,10,Species::Vampire}};
    // 50 x 50: a large board to check timing. Humans scattered with a
    // fixed pseudo-random sequence so the test is the same every run.
    State huge; huge.rows = 50; huge.cols = 50;
    unsigned seed = 12345;
    auto rnd = [&](int n) { seed = seed * 1103515245u + 12345u; return int((seed >> 16) % n); };
    for (int i = 0; i < 40; ++i)
        huge.groups.push_back({rnd(50), rnd(50), 1 + rnd(12), Species::Human});
    huge.groups.push_back({0, 0, 15, Species::Werewolf});
    huge.groups.push_back({49, 49, 15, Species::Vampire});
    {   // drop humans that landed on a monster or on each other
        std::vector<Group> clean; std::set<std::pair<int,int>> used;
        for (auto it = huge.groups.rbegin(); it != huge.groups.rend(); ++it)
            if (used.insert({it->x, it->y}).second) clean.push_back(*it);
        huge.groups = clean;
    }

    int fails = 0;
    fails += playGame("thetrap, vampires first ", trap, Species::Vampire, 0.3, 60);
    fails += playGame("thetrap, werewolves first", trap, Species::Werewolf, 0.3, 60);
    fails += playGame("testmap2, vampires first", big, Species::Vampire, 0.3, 80);
    fails += playGame("thetrap, real 1.7 s limit", trap, Species::Vampire, 1.7, 4);
    fails += playGame("50x50, real 1.7 s limit  ", huge, Species::Vampire, 1.7, 6);
    fails += playGame("50x50, fast games        ", huge, Species::Werewolf, 0.3, 80);

    // Extremes allowed by the protocol (every number is one byte, so 1-255):
    // the biggest grid with the biggest counts, and a small crowded grid.
    auto randomMap = [&](int rows, int cols, int houses, int monsters, int maxHumans) {
        State m; m.rows = rows; m.cols = cols;
        std::set<std::pair<int,int>> used{{0, 0}, {cols - 1, rows - 1}};
        m.groups = {{0, 0, monsters, Species::Werewolf}, {cols - 1, rows - 1, monsters, Species::Vampire}};
        for (int i = 0; i < houses; ++i) {
            int x = rnd(cols), y = rnd(rows);
            if (used.insert({x, y}).second) m.groups.push_back({x, y, 1 + rnd(maxHumans), Species::Human});
        }
        return m;
    };
    fails += playGame("255x255, counts up to 255 ", randomMap(255, 255, 250, 255, 255), Species::Vampire, 1.7, 3);
    fails += playGame("10x10 crowded (90 houses)", randomMap(10, 10, 90, 30, 5), Species::Vampire, 1.7, 3);

    // The fallback must give a legal one-step move even in a corner.
    State corner; corner.rows = 5; corner.cols = 5;
    corner.groups = {{0, 0, 3, Species::Vampire}, {4, 4, 3, Species::Werewolf}};
    Turn t = findBestTurn(corner, Species::Vampire, 0.05);
    std::string err = legalityError(corner, t, Species::Vampire);
    std::cout << "corner position           : " << (err.empty() ? "legal move" : "ILLEGAL: " + err) << "\n";
    if (!err.empty()) ++fails;
    std::cout << (fails ? "SOME TESTS FAILED\n" : "ALL TESTS PASSED\n");
    return fails;
}
