// client.cpp: TCP connection to the game server and the binary protocol.
// Works on macOS / Linux (POSIX sockets) and Windows (Winsock).
//
// Protocol (rules PDF, pages 5-7): every message starts with a 3-letter ASCII
// command; every number after it is ONE unsigned byte.
//   server -> us: SET n m | HUM n (x y)*n | HME x y | MAP n (x y h v w)*n
//                 UPD n (x y h v w)*n | END | BYE
//   us -> server: NME t name | MOV n (fromX fromY count toX toY)*n
#include "twilight.h"

#include <algorithm>
#include <cstdint>
#include <iostream>
#include <string>
#include <vector>

#ifdef _WIN32
  #define NOMINMAX              // stop windows.h from defining min/max macros
  #define WIN32_LEAN_AND_MEAN
  #include <winsock2.h>
  #include <ws2tcpip.h>
  using SocketHandle = SOCKET;
  static const SocketHandle BAD_SOCKET = INVALID_SOCKET;
  static void closeSocket(SocketHandle s) { closesocket(s); }
#else
  #include <arpa/inet.h>
  #include <netdb.h>
  #include <netinet/in.h>
  #include <netinet/tcp.h>
  #include <sys/socket.h>
  #include <unistd.h>
  using SocketHandle = int;
  static const SocketHandle BAD_SOCKET = -1;
  static void closeSocket(SocketHandle s) { close(s); }
#endif

namespace {

SocketHandle handle(long long raw) { return static_cast<SocketHandle>(raw); }

// recv() may return fewer bytes than asked: keep reading until we have them all.
bool recvAll(long long raw, uint8_t* buf, size_t n) {
    size_t got = 0;
    while (got < n) {
        int r = recv(handle(raw), reinterpret_cast<char*>(buf + got), int(n - got), 0);
        if (r <= 0) return false;              // connection closed or error
        got += size_t(r);
    }
    return true;
}

// send() may also send only part of the buffer.
bool sendAll(long long raw, const std::vector<uint8_t>& data) {
    size_t sent = 0;
    while (sent < data.size()) {
        int r = send(handle(raw), reinterpret_cast<const char*>(data.data() + sent),
                     int(data.size() - sent), 0);
        if (r <= 0) return false;
        sent += size_t(r);
    }
    return true;
}

uint8_t toByte(int v) { return uint8_t(std::clamp(v, 0, 255)); }

// One MAP/UPD cell: x, y, humans, vampires, werewolves. A cell holds at most one
// species; all three counts at 0 means the cell is now empty (count = 0).
Group cellToGroup(const uint8_t* c) {
    Group g{c[0], c[1], 0, Species::None};
    if (c[3] > 0)      { g.count = c[3]; g.species = Species::Vampire; }
    else if (c[4] > 0) { g.count = c[4]; g.species = Species::Werewolf; }
    else if (c[2] > 0) { g.count = c[2]; g.species = Species::Human; }
    return g;
}

}  // namespace

bool Client::connectTo(const std::string& ip, int port) {
#ifdef _WIN32
    static bool started = false;
    if (!started) {
        WSADATA wsa;
        if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) return false;
        started = true;
    }
#endif
    addrinfo hints{}, *res = nullptr;
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    if (getaddrinfo(ip.c_str(), std::to_string(port).c_str(), &hints, &res) != 0 || !res) {
        std::cerr << "[client] cannot resolve " << ip << "\n";
        return false;
    }
    SocketHandle s = socket(res->ai_family, res->ai_socktype, res->ai_protocol);
    if (s == BAD_SOCKET) { freeaddrinfo(res); return false; }
    if (connect(s, res->ai_addr, int(res->ai_addrlen)) != 0) {
        std::cerr << "[client] cannot connect to " << ip << ":" << port << "\n";
        closeSocket(s);
        freeaddrinfo(res);
        return false;
    }
    freeaddrinfo(res);

    int one = 1;   // send small messages immediately (no Nagle delay): we have 2 s per move
    setsockopt(s, IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<const char*>(&one), sizeof(one));
    sock_ = static_cast<long long>(s);
    return true;
}

void Client::sendName(const std::string& name) {
    std::string n = name.substr(0, 255);
    std::vector<uint8_t> msg = {'N', 'M', 'E', uint8_t(n.size())};
    msg.insert(msg.end(), n.begin(), n.end());
    if (!sendAll(sock_, msg)) std::cerr << "[client] failed to send NME\n";
}

Message Client::receive() {
    Message m;
    uint8_t cmd[3];
    if (!recvAll(sock_, cmd, 3)) return Message{};          // empty code = connection lost
    m.code = std::string(reinterpret_cast<char*>(cmd), 3);

    if (m.code == "SET" || m.code == "HME") {
        uint8_t b[2];
        if (!recvAll(sock_, b, 2)) return Message{};
        m.header = {b[0], b[1]};                             // SET: rows, cols | HME: x, y
    } else if (m.code == "HUM") {
        uint8_t n;
        if (!recvAll(sock_, &n, 1)) return Message{};
        std::vector<uint8_t> skip(2 * size_t(n));            // houses: read and ignore
        if (n && !recvAll(sock_, skip.data(), skip.size())) return Message{};
    } else if (m.code == "MAP" || m.code == "UPD") {
        uint8_t n;
        if (!recvAll(sock_, &n, 1)) return Message{};
        std::vector<uint8_t> cells(5 * size_t(n));
        if (n && !recvAll(sock_, cells.data(), cells.size())) return Message{};
        for (size_t i = 0; i < n; ++i) m.cells.push_back(cellToGroup(&cells[5 * i]));
    } else if (m.code != "END" && m.code != "BYE") {
        std::cerr << "[client] unknown command '" << m.code << "'\n";
    }
    return m;
}

void Client::sendTurn(const Turn& t) {
    size_t n = std::min<size_t>(t.size(), 255);
    std::vector<uint8_t> msg = {'M', 'O', 'V', uint8_t(n)};
    for (size_t i = 0; i < n; ++i) {
        const Move& mv = t[i];
        msg.push_back(toByte(mv.fromX));
        msg.push_back(toByte(mv.fromY));
        msg.push_back(toByte(std::max(1, mv.count)));
        msg.push_back(toByte(mv.toX));
        msg.push_back(toByte(mv.toY));
    }
    if (!sendAll(sock_, msg)) std::cerr << "[client] failed to send MOV\n";
}
