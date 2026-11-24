// main.cpp: the game loop.
//
// Connects to the server, sends our name, then reacts to each message:
//   SET -> grid size        HME -> our starting cell     MAP -> full board
//   UPD -> board changes, then it's our turn: search and send MOV
//   END -> game over (another game may follow)   BYE -> stop
#include "twilight.h"

#include <iostream>
#include <string>

namespace {
const char*  BOT_NAME   = "AlphaBeta";   // shown by the server; change it if you like
const double TIME_LIMIT = 1.2;           // seconds of search per move (the rule is 2 s);
                                         // 0.8 s of margin for the network and a slow PC
}

int main(int argc, char** argv) {
    // Rules PDF: the tournament launcher puts the IP and the port at the END of
    // the command line (e.g. "twilight.exe -myparams 123.123.3.5 6666"),
    // so always read the last two arguments.
    std::string ip = argc >= 3 ? argv[argc - 2] : "127.0.0.1";
    int port = 5555;
    try { if (argc >= 3) port = std::stoi(argv[argc - 1]); }
    catch (...) { std::cerr << "Port must be a number, using 5555\n"; }

    Client client;
    if (!client.connectTo(ip, port)) { std::cerr << "Cannot connect to " << ip << ":" << port << "\n"; return 1; }
    client.sendName(BOT_NAME);

    State state;
    Species me = Species::None;
    int homeX = -1, homeY = -1;

    while (true) {
        Message m = client.receive();
        if (m.code.empty() || m.code == "BYE") break;          // connection closed / finished

        if (m.code == "END") {                                 // keep the grid size: only the
            state.groups.clear();                              // creatures belong to the old game
            me = Species::None;
        } else if (m.code == "SET" && m.header.size() == 2) {
            state.rows = m.header[0];
            state.cols = m.header[1];
        } else if (m.code == "HME" && m.header.size() == 2) {
            homeX = m.header[0];
            homeY = m.header[1];
        } else if (m.code == "MAP") {
            applyMap(state, m.cells);
            for (const Group& g : state.groups)                // which species are we?
                if (g.x == homeX && g.y == homeY) me = g.species;
            std::cerr << "[main] new game, we are "
                      << (me == Species::Vampire ? "vampires" : me == Species::Werewolf ? "werewolves" : "unknown")
                      << "\n";
        } else if (m.code == "UPD") {
            applyUpdate(state, m.cells);
            client.sendTurn(findBestTurn(state, me, TIME_LIMIT));
        }
    }
    return 0;
}
