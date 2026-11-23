// moves.cpp: candidate move generation + move ordering.
//
// Splitting makes the number of possible turns explode, so we do NOT generate
// every turn. Instead, for each of our groups we build a few sensible "options"
// (stay, move whole group to a neighbour, or split toward two food targets),
// score each option with fast battle-aware rules, then combine options into a
// short list of legal turns, best first. Best-first ordering makes alpha-beta
// prune much more, so the search goes deeper in the same 2 seconds.
#include "twilight.h"

#include <algorithm>
#include <cmath>
#include <set>
#include <tuple>
#include <utility>
#include <vector>

namespace {

// ---------- Tuning knobs (try changing these in self-play) ----------
// The rules PDF says a MOV holds "0 <= n <= number of groups" moves. If the
// official server ever rejects a split (n bigger than our number of groups),
// set this to false: the bot then only moves whole groups.
constexpr bool   ALLOW_SPLITS      = true;
constexpr int    MAX_TURNS         = 20;    // turns returned per position (branching factor)
constexpr int    MAX_OPTIONS_SOLO  = 9;     // options kept when we have 1 group
constexpr double W_EAT             = 10.0;  // eating humans now
constexpr double W_KILL            = 12.0;  // sure-killing an enemy group now
constexpr double W_GAMBLE          = 8.0;   // penalty per creature sent into a random battle
constexpr double W_SUICIDE         = 20.0;  // penalty per creature walking into a sure loss
constexpr double W_EXPOSED         = 15.0;  // ending next to an enemy that can sure-kill us
constexpr double W_FOOD            = 3.0;   // getting closer to safe food
constexpr double W_PREY            = 2.0;   // getting closer to enemy groups we can sure-kill
constexpr double W_FLEE            = 4.0;   // getting away from enemy groups that can sure-kill us
constexpr double W_MERGE           = 2.0;   // joining one of our own groups
constexpr double W_CENTRE          = 0.1;   // tiny tie-breaker: prefer the centre
constexpr double W_SPLIT           = 5.0;   // cost of splitting (two small groups are weaker)

int dist(int x1, int y1, int x2, int y2) {          // Chebyshev: diagonals count as 1
    return std::max(std::abs(x1 - x2), std::abs(y1 - y2));
}
int sgn(int v) { return (v > 0) - (v < 0); }
bool sureKill(int attackers, int defenders) { return attackers >= 1.5 * defenders; }

const Group* groupAt(const State& s, int x, int y) {
    for (const Group& g : s.groups)
        if (g.x == x && g.y == y && g.count > 0) return &g;
    return nullptr;
}

// One choice for one of our groups: 0 moves (stay), 1 move, or 2 moves (split).
struct Option {
    std::vector<Move> moves;
    double score = 0;
};

// Value of having `n` of our creatures standing on (x, y) after our move,
// ignoring what was already on that cell. `ignoreEnemy` = the enemy group we are
// attacking this turn (it will be gone, so it is not a threat).
double positionValue(const State& s, Species who, int x, int y, int n,
                     const Group* ignoreEnemy) {
    Species enemy = enemyOf(who);
    double v = 0;

    double bestFood = 0, bestPrey = 0, threat = 0;
    for (const Group& g : s.groups) {
        if (&g == ignoreEnemy || g.count <= 0) continue;
        int d = std::max(1, dist(x, y, g.x, g.y));

        if (g.species == Species::Human && g.count < n) {   // strictly more: equal is a coin flip
            // Food race: the enemy moves right after us, so it wins ties.
            bool enemyFirst = false;
            for (const Group& e : s.groups)
                if (e.species == enemy && e.count > g.count &&
                    dist(e.x, e.y, g.x, g.y) <= dist(x, y, g.x, g.y)) { enemyFirst = true; break; }
            if (!enemyFirst) bestFood = std::max(bestFood, double(g.count) / d);
        } else if (g.species == enemy) {
            if (sureKill(n, g.count))      bestPrey = std::max(bestPrey, double(g.count) / d);
            else if (sureKill(g.count, n)) {
                threat += double(n) / d;
                if (d <= 1) v -= W_EXPOSED * n;            // it eats us next turn
            } else if (g.count > n && d <= 1) {
                v -= W_GAMBLE * n;                         // bigger: it would probably win a fight
            }
        }
    }
    v += W_FOOD * bestFood + W_PREY * bestPrey - W_FLEE * threat;

    double cx = (s.cols - 1) / 2.0, cy = (s.rows - 1) / 2.0;
    double maxD = std::max(1.0, std::max(cx, cy));
    v -= W_CENTRE * n * std::max(std::abs(x - cx), std::abs(y - cy)) / maxD;
    return v;
}

// Immediate result of sending `n` creatures onto (x, y): eat, kill, gamble, merge.
// Returns the score and which enemy group (if any) is removed.
double arrivalValue(const State& s, Species who, int x, int y, int n, const Group** killed) {
    *killed = nullptr;
    const Group* t = groupAt(s, x, y);
    if (!t) return 0;
    if (t->species == who) return W_MERGE;
    if (t->species == Species::Human) {
        if (n > t->count) return W_EAT * t->count;           // equal = coin flip on the servers
        return -W_GAMBLE * n;
    }
    if (sureKill(n, t->count)) { *killed = t; return W_KILL * t->count; }
    if (sureKill(t->count, n)) return -W_SUICIDE * n;
    return -W_GAMBLE * n;
}

// Size we will have on the target after arriving (used for safety checks).
int sizeAfter(const State& s, Species who, int x, int y, int n) {
    const Group* t = groupAt(s, x, y);
    if (!t) return n;
    if (t->species == who) return n + t->count;
    if (t->species == Species::Human && n > t->count) return n + t->count;
    return n;
}

// Score of moving `n` creatures from group g to (x, y), relative to staying put.
double moveScore(const State& s, Species who, const Group& g, int x, int y, int n) {
    const Group* killed = nullptr;
    double v = arrivalValue(s, who, x, y, n, &killed);
    v += positionValue(s, who, x, y, sizeAfter(s, who, x, y, n), killed);
    v -= positionValue(s, who, g.x, g.y, g.count, nullptr) * (double(n) / g.count);
    return v;
}

std::vector<Option> optionsForGroup(const State& s, const Group& g, Species who) {
    std::vector<Option> opts;
    opts.push_back(Option{});                       // stay (score 0 = baseline)

    // 1) Whole group to each of the 8 neighbours.
    for (int dx = -1; dx <= 1; ++dx)
        for (int dy = -1; dy <= 1; ++dy) {
            if (!dx && !dy) continue;
            int x = g.x + dx, y = g.y + dy;
            if (x < 0 || y < 0 || x >= s.cols || y >= s.rows) continue;
            Option o;
            o.moves.push_back({g.x, g.y, g.count, x, y});
            o.score = moveScore(s, who, g, x, y, g.count);
            opts.push_back(o);
        }

    // 2) Split toward two different safe human targets (each part big enough).
    if (ALLOW_SPLITS && g.count >= 2) {
        // Only food we reach before any enemy group big enough to eat it
        // (the enemy moves right after us, so it wins ties).
        std::vector<const Group*> food;
        for (const Group& h : s.groups) {
            if (h.species != Species::Human || h.count <= 0 || h.count + 1 >= g.count) continue;
            int myD = dist(g.x, g.y, h.x, h.y) - 1;          // after our first step
            bool lost = false;
            for (const Group& e : s.groups)
                if (e.species == enemyOf(who) && e.count > h.count &&
                    dist(e.x, e.y, h.x, h.y) <= myD) { lost = true; break; }
            if (!lost) food.push_back(&h);
        }
        std::sort(food.begin(), food.end(), [&](const Group* a, const Group* b) {
            return double(a->count) / std::max(1, dist(g.x, g.y, a->x, a->y)) >
                   double(b->count) / std::max(1, dist(g.x, g.y, b->x, b->y));
        });
        if (food.size() > 3) food.resize(3);

        for (size_t i = 0; i < food.size(); ++i)
            for (size_t j = 0; j < food.size(); ++j) {
                if (i == j) continue;
                const Group *a = food[i], *b = food[j];
                int n1 = a->count + 1, n2 = g.count - n1;  // one more than a (equal is a coin flip), the rest to b
                if (n2 <= b->count) continue;
                int x1 = g.x + sgn(a->x - g.x), y1 = g.y + sgn(a->y - g.y);
                int x2 = g.x + sgn(b->x - g.x), y2 = g.y + sgn(b->y - g.y);
                if (x1 == x2 && y1 == y2) continue;        // same step: not a real split
                Option o;
                o.moves.push_back({g.x, g.y, n1, x1, y1});
                o.moves.push_back({g.x, g.y, n2, x2, y2});
                o.score = moveScore(s, who, g, x1, y1, n1) + moveScore(s, who, g, x2, y2, n2)
                          - W_SPLIT;
                opts.push_back(o);
            }
    }

    std::sort(opts.begin(), opts.end(),
              [](const Option& a, const Option& b) { return a.score > b.score; });
    return opts;
}

// A turn is legal if it moves something and no cell is both a source and a target.
bool legal(const Turn& t) {
    if (t.empty()) return false;
    std::set<std::pair<int, int>> sources;
    for (const Move& m : t) sources.insert({m.fromX, m.fromY});
    for (const Move& m : t)
        if (sources.count({m.toX, m.toY})) return false;
    return true;
}

std::vector<std::tuple<int, int, int, int, int>> key(const Turn& t) {
    std::vector<std::tuple<int, int, int, int, int>> k;
    for (const Move& m : t) k.emplace_back(m.fromX, m.fromY, m.count, m.toX, m.toY);
    std::sort(k.begin(), k.end());
    return k;
}

}  // namespace

std::vector<Turn> generateTurns(const State& s, Species who) {
    std::vector<const Group*> mine;
    for (const Group& g : s.groups)
        if (g.species == who && g.count > 0) mine.push_back(&g);
    if (mine.empty()) return {};

    // Fewer options per group when we have many groups (keeps the list short).
    int keep = std::max(2, MAX_OPTIONS_SOLO / int(mine.size()));
    std::vector<std::vector<Option>> opts;
    for (const Group* g : mine) {
        auto o = optionsForGroup(s, *g, who);
        if (int(o.size()) > keep + 1) o.resize(keep + 1);
        opts.push_back(o);
    }

    std::vector<std::pair<double, Turn>> cands;
    auto add = [&](const std::vector<int>& pick) {
        Turn t; double sc = 0;
        for (size_t i = 0; i < pick.size(); ++i) {
            const Option& o = opts[i][pick[i]];
            t.insert(t.end(), o.moves.begin(), o.moves.end());
            sc += o.score;
        }
        if (legal(t)) cands.push_back({sc, t});
    };

    // Best option of every group, then vary one group at a time.
    std::vector<int> best(mine.size(), 0);
    add(best);
    for (size_t i = 0; i < mine.size(); ++i)
        for (size_t k = 1; k < opts[i].size(); ++k) {
            std::vector<int> p = best; p[i] = int(k); add(p);
        }
    // Single-group turns (everyone else stays): always legal, guarantees >= 1 turn.
    for (size_t i = 0; i < mine.size(); ++i)
        for (size_t k = 0; k < opts[i].size(); ++k) {
            if (opts[i][k].moves.empty()) continue;
            cands.push_back({opts[i][k].score, opts[i][k].moves});
        }

    std::sort(cands.begin(), cands.end(),
              [](const auto& a, const auto& b) { return a.first > b.first; });
    std::vector<Turn> out;
    std::set<std::vector<std::tuple<int, int, int, int, int>>> seen;
    for (auto& c : cands) {
        if (!seen.insert(key(c.second)).second) continue;
        out.push_back(c.second);
        if (int(out.size()) >= MAX_TURNS) break;
    }
    return out;
}
