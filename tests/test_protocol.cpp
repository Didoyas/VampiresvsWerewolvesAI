// test_protocol.cpp: a fake game server that plays two full games against the
// REAL twilight program, started the way the tournament launcher does it
// ("twilight -launcherparams 127.0.0.1 <port>"). It checks every message the
// bot sends, that every move is legal, and that every answer arrives within 2 s.
// The server's own side (the opponent) is played by the same search, quickly.
// POSIX only (macOS / Linux).
#include "twilight.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <map>
#include <set>
#include <string>
#include <thread>

#ifndef TWILIGHT_BIN
#error "TWILIGHT_BIN must be set by CMake"
#endif

namespace {

int fails = 0;
void fail(const std::string& why) { std::cout << "  FAIL  " << why << "\n"; ++fails; }

bool recvAll(int s, uint8_t* b, size_t n) {
    size_t got = 0;
    while (got < n) { ssize_t r = recv(s, b + got, n - got, 0); if (r <= 0) return false; got += size_t(r); }
    return true;
}
void sendAll(int s, const std::vector<uint8_t>& d) { send(s, d.data(), d.size(), 0); }

std::vector<uint8_t> cellsMessage(const char* cmd, const std::vector<std::array<int, 5>>& cells) {
    std::vector<uint8_t> m = {uint8_t(cmd[0]), uint8_t(cmd[1]), uint8_t(cmd[2]), uint8_t(cells.size())};
    for (auto& c : cells) for (int v : c) m.push_back(uint8_t(v));
    return m;
}

std::array<int, 5> toCell(int x, int y, const Group* g) {
    std::array<int, 5> c{x, y, 0, 0, 0};
    if (!g) return c;
    if (g->species == Species::Human) c[2] = g->count;
    if (g->species == Species::Vampire) c[3] = g->count;
    if (g->species == Species::Werewolf) c[4] = g->count;
    return c;
}

// Cells that differ between two boards, as UPD tuples.
std::vector<std::array<int, 5>> diff(const State& a, const State& b) {
    std::map<std::pair<int, int>, const Group*> A, B;
    for (auto& g : a.groups) A[{g.x, g.y}] = &g;
    for (auto& g : b.groups) B[{g.x, g.y}] = &g;
    std::set<std::pair<int, int>> keys;
    for (auto& kv : A) keys.insert(kv.first);
    for (auto& kv : B) keys.insert(kv.first);
    std::vector<std::array<int, 5>> out;
    for (auto& k : keys) {
        const Group* ga = A.count(k) ? A[k] : nullptr;
        const Group* gb = B.count(k) ? B[k] : nullptr;
        if (toCell(k.first, k.second, ga) != toCell(k.first, k.second, gb))
            out.push_back(toCell(k.first, k.second, gb));
    }
    return out;
}

std::string legality(const State& s, const Turn& t, Species who) {
    if (t.empty()) return "empty MOV";
    std::set<std::pair<int, int>> sources;
    std::map<std::pair<int, int>, int> used;
    for (auto& m : t) sources.insert({m.fromX, m.fromY});
    for (auto& m : t) {
        if (m.count < 1) return "count < 1";
        if (m.toX < 0 || m.toY < 0 || m.toX >= s.cols || m.toY >= s.rows) return "off the board";
        if (std::max(std::abs(m.toX - m.fromX), std::abs(m.toY - m.fromY)) != 1) return "not one step";
        if (sources.count({m.toX, m.toY})) return "target is also a source";
        used[{m.fromX, m.fromY}] += m.count;
    }
    for (auto& [p, n] : used) {
        bool ok = false;
        for (auto& g : s.groups) if (g.x == p.first && g.y == p.second && g.species == who && g.count >= n) ok = true;
        if (!ok) return "moves creatures it doesn't have";
    }
    return "";
}

// Plays one game: the bot is `botSide`, the server plays the other side.
void playGame(int c, Species botSide, int turns) {
    State s; s.rows = 5; s.cols = 10;
    s.groups = {{2,2,4,Species::Human},{9,0,2,Species::Human},{9,2,1,Species::Human},
                {9,4,2,Species::Human},{4,1,4,Species::Werewolf},{4,3,4,Species::Vampire}};
    const Group* home = nullptr;
    for (auto& g : s.groups) if (g.species == botSide) home = &g;

    sendAll(c, {'S','E','T', 5, 10});
    sendAll(c, {'H','U','M', 4, 2,2, 9,0, 9,2, 9,4});
    sendAll(c, {'H','M','E', uint8_t(home->x), uint8_t(home->y)});
    std::vector<std::array<int, 5>> all;
    for (auto& g : s.groups) all.push_back(toCell(g.x, g.y, &g));
    sendAll(c, cellsMessage("MAP", all));

    std::vector<std::array<int, 5>> changes;    // first UPD: no changes (the bot moves first)
    double slowest = 0;
    for (int t = 0; t < turns && !isTerminal(s); ++t) {
        auto t0 = std::chrono::steady_clock::now();
        sendAll(c, cellsMessage("UPD", changes));

        uint8_t head[4];
        if (!recvAll(c, head, 4) || std::string((char*)head, 3) != "MOV") { fail("no MOV received"); return; }
        std::vector<uint8_t> body(5 * size_t(head[3]));
        if (head[3] && !recvAll(c, body.data(), body.size())) { fail("MOV cut short"); return; }
        double dt = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
        slowest = std::max(slowest, dt);
        if (dt > 2.0) fail("answer took " + std::to_string(dt) + " s (over 2 s)");

        Turn mv;
        for (size_t i = 0; i < head[3]; ++i)
            mv.push_back({body[5*i], body[5*i+1], body[5*i+2], body[5*i+3], body[5*i+4]});
        std::string err = legality(s, mv, botSide);
        if (!err.empty()) { fail("illegal MOV on turn " + std::to_string(t) + ": " + err); return; }

        State before = s;
        s = applyTurn(s, mv, botSide);
        if (!isTerminal(s)) s = applyTurn(s, findBestTurn(s, enemyOf(botSide), 0.1), enemyOf(botSide));
        changes = diff(before, s);
    }
    std::cout << "  ok    game as " << (botSide == Species::Vampire ? "vampires  " : "werewolves")
              << ": every MOV legal, slowest answer " << slowest << " s\n";
}

}  // namespace

int main() {
    std::cout << "protocol (fake server vs the real twilight program)\n";
    int srv = socket(AF_INET, SOCK_STREAM, 0);
    int one = 1;
    setsockopt(srv, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = 0;                                   // let the system pick a free port
    bind(srv, (sockaddr*)&addr, sizeof(addr));
    listen(srv, 1);
    socklen_t len = sizeof(addr);
    getsockname(srv, (sockaddr*)&addr, &len);
    int port = ntohs(addr.sin_port);

    // Start the bot exactly like the launcher: extra parameters first, IP and port last.
    std::string cmd = std::string("\"") + TWILIGHT_BIN + "\" -launcherparams 127.0.0.1 " +
                      std::to_string(port) + " 2>/dev/null";
    int botExit = -1;
    std::thread bot([&] { botExit = std::system(cmd.c_str()); });

    int c = accept(srv, nullptr, nullptr);
    timeval tv{5, 0};                                    // never wait more than 5 s for the bot
    setsockopt(c, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    uint8_t nme[4];
    if (!recvAll(c, nme, 4) || std::string((char*)nme, 3) != "NME") fail("first message is not NME");
    else {
        std::vector<uint8_t> name(nme[3]);
        if (nme[3]) recvAll(c, name.data(), name.size());
        std::cout << "  ok    NME received: \"" << std::string(name.begin(), name.end()) << "\"\n";
    }

    playGame(c, Species::Vampire, 8);
    sendAll(c, {'E','N','D'});
    playGame(c, Species::Werewolf, 8);                   // second game after END, other species
    sendAll(c, {'E','N','D'});
    sendAll(c, {'B','Y','E'});

    bot.join();
    if (botExit != 0) fail("the bot did not exit cleanly after BYE");
    else std::cout << "  ok    bot exited cleanly after BYE\n";
    close(c);
    close(srv);
    std::cout << (fails ? "PROTOCOL TESTS FAILED\n" : "ALL PROTOCOL TESTS PASSED\n");
    return fails ? 1 : 0;
}
