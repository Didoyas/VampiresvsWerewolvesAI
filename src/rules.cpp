// rules.cpp: keeping the board up to date and simulating a turn.
//
// Rules from the assignment PDF (pages 3-4), when creatures arrive on a cell:
//   empty or own species      -> they merge
//   humans,  we >= humans     -> every human converts to our species
//   humans,  we <  humans     -> random battle
// "At least as numerous" converts for sure, as in the PDF; tests on the professor's
// server confirmed that an attack with exactly as many creatures as humans always
// converts them. (The open-source Go server makes that case a coin flip instead.)
//   enemies, we >= 1.5 x them -> they all die, we all survive
//   enemies, they >= 1.5 x us -> all our attackers die
//   enemies, otherwise        -> random battle
// Random battle: P = 0.5 if E1 == E2, E1 / (2 E2) if E1 < E2, E1 / E2 - 0.5 otherwise.
// Winners: each attacker survives with probability P (and, against humans, each
// human converts with probability P). Losers: each defender survives with 1 - P.
//
// The search needs ONE board per move, so random battles are resolved
// pessimistically for the attacker: against humans it wins only if P > 0.5 (never
// true when it is outnumbered), against monsters it always loses unless it has the
// 1.5x needed for a sure kill. Survivors are the expected number, rounded. So the
// bot never counts on luck.
#include "twilight.h"

#include <algorithm>
#include <cmath>
#include <vector>

namespace {

int findCell(const State& s, int x, int y) {
    for (size_t i = 0; i < s.groups.size(); ++i)
        if (s.groups[i].x == x && s.groups[i].y == y && s.groups[i].count > 0) return int(i);
    return -1;
}

void removeEmpty(State& s) {
    s.groups.erase(std::remove_if(s.groups.begin(), s.groups.end(),
                                  [](const Group& g) { return g.count <= 0; }),
                   s.groups.end());
}

double winProbability(double attackers, double defenders) {
    if (attackers == defenders) return 0.5;
    if (attackers < defenders)  return attackers / (2 * defenders);
    return attackers / defenders - 0.5;
}

// Resolve `n` creatures of species `who` arriving on cell `d`.
void resolveArrival(Group& d, int n, Species who) {
    if (d.species == who) { d.count += n; return; }

    if (d.species == Species::Human) {
        if (n >= d.count) { d.count += n; d.species = who; return; }   // sure conversion
        double p = winProbability(n, d.count);
        if (p > 0.5) { d.count = int(std::lround(p * (n + d.count))); d.species = who; }
        else           d.count = int(std::lround((1 - p) * d.count));  // humans stay
        return;
    }

    // Enemy monsters.
    if (n >= 1.5 * d.count) { d.count = n; d.species = who; return; }  // we wipe them out
    if (d.count >= 1.5 * n) return;                                    // they wipe us out
    // Any other monster fight is a gamble. The PDF gives the attacker P = E1/E2 - 0.5
    // when it has more, but the Go server (copied from the official one) returns 0.5
    // there. Either way the bot doesn't count on luck: the attacker is assumed to lose.
    double p = winProbability(n, d.count);
    d.count = int(std::lround((1 - p) * d.count));
}

}  // namespace

void applyMap(State& s, const std::vector<Group>& cells) {
    s.groups.clear();
    for (const Group& g : cells)
        if (g.count > 0 && g.species != Species::None) s.groups.push_back(g);
}

void applyUpdate(State& s, const std::vector<Group>& changedCells) {
    for (const Group& c : changedCells) {
        int i = findCell(s, c.x, c.y);
        if (i >= 0) s.groups[i] = c;                 // replace (count 0 = now empty)
        else if (c.count > 0) s.groups.push_back(c);  // new cell
    }
    removeEmpty(s);
}

State applyTurn(const State& s, const Turn& t, Species who) {
    State out = s;

    // 1) Take the moving creatures off their cells, and total them per target cell.
    struct Arrival { int x, y, n; };
    std::vector<Arrival> arrivals;
    for (const Move& m : t) {
        int i = findCell(out, m.fromX, m.fromY);
        if (i < 0 || out.groups[i].species != who) continue;   // not ours: ignore
        int n = std::min(m.count, out.groups[i].count);
        if (n <= 0) continue;
        out.groups[i].count -= n;
        auto a = std::find_if(arrivals.begin(), arrivals.end(),
                              [&](const Arrival& r) { return r.x == m.toX && r.y == m.toY; });
        if (a != arrivals.end()) a->n += n;
        else arrivals.push_back({m.toX, m.toY, n});
    }

    // 2) Resolve what happens on each target cell.
    for (const Arrival& a : arrivals) {
        int i = findCell(out, a.x, a.y);
        if (i < 0) out.groups.push_back({a.x, a.y, a.n, who});
        else resolveArrival(out.groups[i], a.n, who);
    }

    removeEmpty(out);
    return out;
}

int totalCount(const State& s, Species who) {
    int n = 0;
    for (const Group& g : s.groups) if (g.species == who) n += g.count;
    return n;
}

bool isTerminal(const State& s) {
    return totalCount(s, Species::Vampire) == 0 || totalCount(s, Species::Werewolf) == 0;
}
