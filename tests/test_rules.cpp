// test_rules.cpp: checks rules.cpp against the cases in the rules PDF.
#include "twilight.h"

#include <iostream>
#include <string>

static int fails = 0;

static void check(const std::string& name, bool ok) {
    std::cout << (ok ? "  ok    " : "  FAIL  ") << name << "\n";
    if (!ok) ++fails;
}

static const Group* at(const State& s, int x, int y) {
    for (const Group& g : s.groups) if (g.x == x && g.y == y) return &g;
    return nullptr;
}

static State board(std::vector<Group> groups) {
    State s; s.rows = 5; s.cols = 5; s.groups = std::move(groups); return s;
}

int main() {
    const Species V = Species::Vampire, W = Species::Werewolf, H = Species::Human;
    std::cout << "rules.cpp\n";

    // Converting humans: at least as many as them (confirmed on the professor's server).
    State s = applyTurn(board({{1,1,5,V}, {2,2,4,H}}), {{1,1,5,2,2}}, V);
    check("5 vampires on 4 humans -> 9 vampires", at(s,2,2) && at(s,2,2)->species == V && at(s,2,2)->count == 9);
    check("the source cell is now empty", at(s,1,1) == nullptr);

    s = applyTurn(board({{1,1,4,V}, {2,2,4,H}}), {{1,1,4,2,2}}, V);
    check("4 vampires on 4 humans (equal) -> sure conversion, 8 vampires", at(s,2,2) && at(s,2,2)->species == V && at(s,2,2)->count == 8);

    s = applyTurn(board({{1,1,3,V}, {2,2,4,H}}), {{1,1,3,2,2}}, V);
    check("3 vampires on 4 humans -> random battle, humans most likely stay",
          at(s,2,2) && at(s,2,2)->species == H);

    // Monsters: 1.5x either way.
    s = applyTurn(board({{1,1,6,V}, {2,2,4,W}}), {{1,1,6,2,2}}, V);
    check("6 vampires on 4 werewolves (1.5x) -> 6 vampires", at(s,2,2) && at(s,2,2)->species == V && at(s,2,2)->count == 6);

    s = applyTurn(board({{1,1,4,V}, {2,2,6,W}}), {{1,1,4,2,2}}, V);
    check("4 vampires on 6 werewolves (they are 1.5x) -> attackers die, 6 werewolves stay",
          at(s,2,2) && at(s,2,2)->species == W && at(s,2,2)->count == 6 && totalCount(s, V) == 0);

    s = applyTurn(board({{1,1,5,V}, {2,2,4,W}}), {{1,1,5,2,2}}, V);
    check("5 vampires on 4 werewolves -> a gamble, counted as a loss for the attacker",
          at(s,2,2) && at(s,2,2)->species == W);

    s = applyTurn(board({{1,1,4,V}, {2,2,4,W}}), {{1,1,4,2,2}}, V);
    check("4 vs 4 (P = 0.5) counts as a loss for the attacker", at(s,2,2) && at(s,2,2)->species == W);

    // Splits, merges, and moving only part of a group.
    s = applyTurn(board({{2,2,8,V}}), {{2,2,3,1,1}, {2,2,5,3,3}}, V);
    check("split 8 into 3 + 5", at(s,1,1) && at(s,1,1)->count == 3 && at(s,3,3) && at(s,3,3)->count == 5 && !at(s,2,2));

    s = applyTurn(board({{1,1,3,V}, {3,1,2,V}}), {{1,1,3,2,1}, {3,1,2,2,1}}, V);
    check("two groups meeting on one cell merge into 5", at(s,2,1) && at(s,2,1)->count == 5);

    s = applyTurn(board({{2,2,8,V}}), {{2,2,3,1,1}}, V);
    check("moving part of a group leaves the rest", at(s,2,2) && at(s,2,2)->count == 5 && at(s,1,1)->count == 3);

    // The input board must not change.
    State before = board({{1,1,5,V}, {2,2,4,H}});
    applyTurn(before, {{1,1,5,2,2}}, V);
    check("applyTurn leaves its input unchanged", at(before,1,1)->count == 5 && at(before,2,2)->species == H);

    // Board updates from the server.
    State u = board({{1,1,4,V}, {2,2,4,H}});
    applyUpdate(u, {{1,1,0,Species::None}, {2,2,8,V}});
    check("UPD: emptied cell removed, changed cell replaced", !at(u,1,1) && at(u,2,2)->species == V && at(u,2,2)->count == 8);
    applyUpdate(u, {{4,4,3,W}});
    check("UPD: new cell added", at(u,4,4) && at(u,4,4)->species == W);

    State mp; applyMap(mp, {{0,0,2,H}, {1,1,0,Species::None}, {3,3,5,V}});
    check("MAP: empty cells are skipped", mp.groups.size() == 2);

    check("isTerminal when a species is gone", isTerminal(board({{1,1,3,V}, {2,2,1,H}})));
    check("not terminal with both species", !isTerminal(board({{1,1,3,V}, {4,4,3,W}})));

    // Evaluation basics.
    State e = board({{1,1,8,V}, {4,4,4,W}});
    check("eval: bigger army scores positive", evaluate(e, V, 0, V) > 0 && evaluate(e, W, 0, V) < 0);
    State race = board({{0,2,4,V}, {4,2,4,W}, {2,2,3,H}});
    check("eval: tempo decides a tied race for humans",
          evaluate(race, V, 0, V) > evaluate(race, V, 0, W));
    check("eval: a win scores about +1,000,000", evaluate(board({{1,1,3,V}}), V, 2, W) > 5e5);

    std::cout << (fails ? "RULES TESTS FAILED\n" : "ALL RULES TESTS PASSED\n");
    return fails ? 1 : 0;
}
