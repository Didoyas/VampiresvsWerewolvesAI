// eval.cpp: the evaluation function (heuristic).
//
// Gives a board one number from `me`'s point of view: positive = good for us.
// The search calls it at the end of every line it explores, hundreds of
// thousands of times per move, so it only loops over groups (never over cells).
//
// Terms, from most to least important:
//   1. win / loss            +-1,000,000, minus ply so faster wins score higher
//   2. material              our creatures - theirs, plus a ratio term (8 vs 4 is
//                            safer than 108 vs 104)
//   3. food race             each human group goes to whoever can eat it and gets
//                            there first; ties go to the side about to move (tempo)
//   4. attack or run         prey (we are >= 1.5x) close = good, predators close = bad,
//                            and much more so when the attacker is the side to move
//   5. game phase            once all humans are gone, fights count double
//   6. small extras          fewer, bigger groups; a tiny preference for the centre
#include "twilight.h"

#include <algorithm>
#include <climits>
#include <cmath>
#include <vector>

namespace {

// Current weights (defaults in twilight.h). One copy per thread for the tuner.
thread_local EvalWeights W;

int dist(const Group& a, const Group& b) {             // Chebyshev: diagonals count as 1
    return std::max(std::abs(a.x - b.x), std::abs(a.y - b.y));
}
bool sureKill(int attackers, int defenders) { return attackers >= 1.5 * defenders; }

// Shortest distance from any group in `side` big enough to convert `h` for sure.
int eatDistance(const std::vector<const Group*>& side, const Group& h) {
    int best = INT_MAX;
    for (const Group* g : side)
        if (g->count > h.count) best = std::min(best, dist(*g, h));   // equal = coin flip
    return best;
}

double centreScore(const State& s, const std::vector<const Group*>& side) {
    double cx = (s.cols - 1) / 2.0, cy = (s.rows - 1) / 2.0;
    double maxD = std::max(1.0, std::max(cx, cy)), v = 0;
    for (const Group* g : side)
        v -= g->count * std::max(std::abs(g->x - cx), std::abs(g->y - cy)) / maxD;
    return v;
}

}  // namespace

void setEvalWeights(const EvalWeights& w) { W = w; }
EvalWeights getEvalWeights() { return W; }

double evaluate(const State& s, Species me, int ply, Species toMove) {
    const Species enemy = enemyOf(me);
    std::vector<const Group*> mine, theirs, humans;
    int myCount = 0, theirCount = 0;
    for (const Group& g : s.groups) {
        if (g.count <= 0) continue;
        if (g.species == me)                  { mine.push_back(&g);   myCount += g.count; }
        else if (g.species == enemy)          { theirs.push_back(&g); theirCount += g.count; }
        else if (g.species == Species::Human)   humans.push_back(&g);
    }

    // 1. Win / loss.
    if (myCount == 0 && theirCount == 0) return 0;
    if (theirCount == 0) return  1e6 - ply;
    if (myCount == 0)    return -1e6 + ply;

    // 2. Material.
    double score = W.material * (myCount - theirCount)
                 + W.ratio * double(myCount - theirCount) / (myCount + theirCount);

    // 3. Food race, with tempo deciding ties.
    for (const Group* h : humans) {
        int dMe = eatDistance(mine, *h), dThem = eatDistance(theirs, *h);
        if (dMe == INT_MAX && dThem == INT_MAX) continue;      // nobody is big enough yet
        bool oursFirst = dMe < dThem || (dMe == dThem && toMove == me);
        int d = std::max(1, oursFirst ? dMe : dThem);
        score += (oursFirst ? 1 : -1) * W.food * h->count / d;
    }

    // 4. Attack or run, and 5. game phase (no humans left = pure fight).
    double fight = humans.empty() ? 2.0 : 1.0;
    for (const Group* a : mine)
        for (const Group* b : theirs) {
            int d = std::max(1, dist(*a, *b));
            if (sureKill(a->count, b->count)) {
                score += fight * W.prey * b->count / d;
                if (d == 1 && toMove == me) score += W.strike * b->count;     // we eat them next
            } else if (sureKill(b->count, a->count)) {
                score -= fight * W.danger * a->count / d;
                if (d == 1 && toMove == enemy) score -= W.strike * a->count;  // they eat us next
            } else if (d == 1) {
                // Neither side wins for sure: a fight here is a gamble, worse for the smaller one.
                score -= W.risk * (a->count < b->count ? a->count : -b->count);
            }
        }

    // 6. Small extras.
    score -= W.groups * (double(mine.size()) - double(theirs.size()));
    score += W.centre * (centreScore(s, mine) - centreScore(s, theirs));
    return score;
}
