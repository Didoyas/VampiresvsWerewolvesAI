#include <array>
#include <chrono>
#include <cerrno>
#include <cstdint>
#include <csignal>
#include <cstdlib>
#include <exception>
#include <iostream>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#ifdef _WIN32
    #ifndef NOMINMAX
        #define NOMINMAX
    #endif
    #include <winsock2.h>
    #include <ws2tcpip.h>
    using SocketHandle = SOCKET;
    static constexpr SocketHandle INVALID_SOCKET_HANDLE = INVALID_SOCKET;
#else
    #include <arpa/inet.h>
    #include <netdb.h>
    #include <sys/socket.h>
    #include <sys/types.h>
    #include <unistd.h>
    using SocketHandle = int;
    static constexpr SocketHandle INVALID_SOCKET_HANDLE = -1;
#endif

namespace vvw {

// -----------------------------------------------------------------------------
// Protocol data types
// -----------------------------------------------------------------------------

enum class Command {
    Set,
    Hum,
    Hme,
    Map,
    Upd,
    End,
    Bye
};

struct Position {
    std::uint8_t x{};
    std::uint8_t y{};
};

// One five-byte record used by MAP/UPD:
// x, y, humans, vampires, werewolves.
struct CellUpdate {
    std::uint8_t x{};
    std::uint8_t y{};
    std::uint8_t humans{};
    std::uint8_t vampires{};
    std::uint8_t werewolves{};
};

// One five-byte record used by MOV:
// source x, source y, number of creatures, target x, target y.
struct Move {
    std::uint8_t sourceX{};
    std::uint8_t sourceY{};
    std::uint8_t number{};
    std::uint8_t targetX{};
    std::uint8_t targetY{};
};

struct Message {
    Command command{};
    std::uint8_t rows{};
    std::uint8_t columns{};
    Position position{};
    std::vector<Position> houses;
    std::vector<CellUpdate> updates;
};

// -----------------------------------------------------------------------------
// Small cross-platform socket wrapper (macOS/Linux/Windows)
// -----------------------------------------------------------------------------

#ifdef _WIN32
class SocketRuntime {
public:
    SocketRuntime() {
        WSADATA data{};
        const int result = WSAStartup(MAKEWORD(2, 2), &data);
        if (result != 0) {
            throw std::runtime_error("WSAStartup failed: " + std::to_string(result));
        }
    }
    ~SocketRuntime() { WSACleanup(); }
    SocketRuntime(const SocketRuntime&) = delete;
    SocketRuntime& operator=(const SocketRuntime&) = delete;
};
#endif

static void closeSocket(SocketHandle socketHandle) noexcept {
    if (socketHandle == INVALID_SOCKET_HANDLE) {
        return;
    }
#ifdef _WIN32
    closesocket(socketHandle);
#else
    ::close(socketHandle);
#endif
}

static int lastSocketError() noexcept {
#ifdef _WIN32
    return WSAGetLastError();
#else
    return errno;
#endif
}

class ClientSocket {
public:
    ClientSocket(const std::string& ip, std::uint16_t port)
        : ip_(ip), port_(port) {
        connectToServer();
    }

    ~ClientSocket() { closeSocket(socket_); }

    ClientSocket(const ClientSocket&) = delete;
    ClientSocket& operator=(const ClientSocket&) = delete;

    void sendNme(const std::string& name) {
        if (name.size() > 255) {
            throw std::runtime_error("AI name must fit in one byte (maximum 255 characters)");
        }

        std::vector<std::uint8_t> packet;
        packet.reserve(4 + name.size());
        appendCommand(packet, "NME");
        packet.push_back(static_cast<std::uint8_t>(name.size()));
        for (unsigned char ch : name) {
            packet.push_back(static_cast<std::uint8_t>(ch));
        }
        sendAll(packet);
    }

    Message readMessage() {
        std::array<std::uint8_t, 3> commandBytes{};
        receiveExact(commandBytes.data(), commandBytes.size());

        const std::string command(commandBytes.begin(), commandBytes.end());
        Message message{};

        if (command == "END") {
            message.command = Command::End;
            return message;
        }
        if (command == "BYE") {
            message.command = Command::Bye;
            return message;
        }

        if (command == "SET") {
            message.command = Command::Set;
            message.rows = readByte();
            message.columns = readByte();
            return message;
        }

        if (command == "HUM") {
            message.command = Command::Hum;
            const std::uint8_t count = readByte();
            message.houses.reserve(count);
            for (std::uint16_t i = 0; i < count; ++i) {
                Position p{};
                p.x = readByte();
                p.y = readByte();
                message.houses.push_back(p);
            }
            return message;
        }

        if (command == "HME") {
            message.command = Command::Hme;
            message.position.x = readByte();
            message.position.y = readByte();
            return message;
        }

        if (command == "MAP" || command == "UPD") {
            message.command = (command == "MAP") ? Command::Map : Command::Upd;
            const std::uint8_t count = readByte();
            message.updates.reserve(count);
            for (std::uint16_t i = 0; i < count; ++i) {
                CellUpdate update{};
                update.x = readByte();
                update.y = readByte();
                update.humans = readByte();
                update.vampires = readByte();
                update.werewolves = readByte();
                message.updates.push_back(update);
            }
            return message;
        }

        throw std::runtime_error("Unknown server command: " + command);
    }

    void sendMoves(const std::vector<Move>& moves) {
        if (moves.size() > 255) {
            throw std::runtime_error("MOV contains too many moves for the one-byte count field");
        }

        std::vector<std::uint8_t> packet;
        packet.reserve(4 + moves.size() * 5);
        appendCommand(packet, "MOV");
        packet.push_back(static_cast<std::uint8_t>(moves.size()));

        for (const Move& move : moves) {
            packet.push_back(move.sourceX);
            packet.push_back(move.sourceY);
            packet.push_back(move.number);
            packet.push_back(move.targetX);
            packet.push_back(move.targetY);
        }

        sendAll(packet);
    }

private:
    std::string ip_;
    std::uint16_t port_{};
    SocketHandle socket_{INVALID_SOCKET_HANDLE};

    static void appendCommand(std::vector<std::uint8_t>& packet, const char* command) {
        packet.push_back(static_cast<std::uint8_t>(command[0]));
        packet.push_back(static_cast<std::uint8_t>(command[1]));
        packet.push_back(static_cast<std::uint8_t>(command[2]));
    }

    void connectToServer() {
        addrinfo hints{};
        hints.ai_family = AF_UNSPEC;
        hints.ai_socktype = SOCK_STREAM;
        hints.ai_protocol = IPPROTO_TCP;

        const std::string service = std::to_string(port_);
        addrinfo* addresses = nullptr;
        const int lookupResult = getaddrinfo(ip_.c_str(), service.c_str(), &hints, &addresses);
        if (lookupResult != 0) {
#ifdef _WIN32
            throw std::runtime_error("getaddrinfo failed: " + std::to_string(lookupResult));
#else
            throw std::runtime_error(std::string("getaddrinfo failed: ") + gai_strerror(lookupResult));
#endif
        }

        int errorCode = 0;
        for (addrinfo* address = addresses; address != nullptr; address = address->ai_next) {
            SocketHandle candidate = ::socket(address->ai_family, address->ai_socktype,
                                              address->ai_protocol);
            if (candidate == INVALID_SOCKET_HANDLE) {
                errorCode = lastSocketError();
                continue;
            }

            if (::connect(candidate, address->ai_addr,
                          static_cast<int>(address->ai_addrlen)) == 0) {
                socket_ = candidate;
                break;
            }

            errorCode = lastSocketError();
            closeSocket(candidate);
        }

        freeaddrinfo(addresses);

        if (socket_ == INVALID_SOCKET_HANDLE) {
            throw std::runtime_error("Could not connect to " + ip_ + ":" + service +
                                     " (socket error " + std::to_string(errorCode) + ")");
        }
    }

    std::uint8_t readByte() {
        std::uint8_t value{};
        receiveExact(&value, 1);
        return value;
    }

    void receiveExact(void* destination, std::size_t length) {
        auto* output = static_cast<std::uint8_t*>(destination);
        std::size_t received = 0;

        while (received < length) {
#ifdef _WIN32
            const int result = ::recv(socket_, reinterpret_cast<char*>(output + received),
                                      static_cast<int>(length - received), 0);
#else
            const ssize_t result = ::recv(socket_, output + received, length - received, 0);
#endif
            if (result == 0) {
                throw std::runtime_error("Server closed the TCP connection");
            }
            if (result < 0) {
#ifdef _WIN32
                const int error = WSAGetLastError();
                if (error == WSAEINTR) continue;
#else
                if (errno == EINTR) continue;
                const int error = errno;
#endif
                throw std::runtime_error("Socket receive failed (error " +
                                         std::to_string(error) + ")");
            }
            received += static_cast<std::size_t>(result);
        }
    }

    void sendAll(const std::vector<std::uint8_t>& packet) {
        std::size_t sent = 0;
        while (sent < packet.size()) {
#ifdef _WIN32
            const int result = ::send(socket_,
                                      reinterpret_cast<const char*>(packet.data() + sent),
                                      static_cast<int>(packet.size() - sent), 0);
#else
            const ssize_t result = ::send(socket_, packet.data() + sent,
                                          packet.size() - sent, 0);
#endif
            if (result == 0) {
                throw std::runtime_error("Socket send returned zero bytes");
            }
            if (result < 0) {
#ifdef _WIN32
                const int error = WSAGetLastError();
                if (error == WSAEINTR) continue;
#else
                if (errno == EINTR) continue;
                const int error = errno;
#endif
                throw std::runtime_error("Socket send failed (error " +
                                         std::to_string(error) + ")");
            }
            sent += static_cast<std::size_t>(result);
        }
    }
};

// -----------------------------------------------------------------------------
// Game-state representation
// -----------------------------------------------------------------------------

enum class Species {
    Unknown,
    Vampires,
    Werewolves
};

struct Cell {
    std::uint8_t humans{};
    std::uint8_t vampires{};
    std::uint8_t werewolves{};
    bool isHouse{false};
};

class GameState {
public:
    void clear() {
        rows_ = 0;
        columns_ = 0;
        cells_.clear();
        houses_.clear();
        hasHomePosition_ = false;
        homePosition_ = {};
        ourSpecies_ = Species::Unknown;
    }

    void apply(const Message& message) {
        switch (message.command) {
            case Command::Set:
                rows_ = message.rows;
                columns_ = message.columns;
                if (rows_ == 0 || columns_ == 0) {
                    throw std::runtime_error("Server sent an empty board in SET");
                }
                cells_.assign(static_cast<std::size_t>(rows_) * columns_, Cell{});
                houses_.clear();
                hasHomePosition_ = false;
                ourSpecies_ = Species::Unknown;
                break;

            case Command::Hum:
                houses_ = message.houses;
                for (const Position& house : houses_) {
                    if (isInside(house.x, house.y)) {
                        at(house.x, house.y).isHouse = true;
                    }
                }
                break;

            case Command::Hme:
                homePosition_ = message.position;
                hasHomePosition_ = true;
                inferOurSpecies();
                break;

            case Command::Map:
            case Command::Upd:
                for (const CellUpdate& update : message.updates) {
                    if (!isInside(update.x, update.y)) {
                        throw std::runtime_error("Server sent an update outside the board");
                    }
                    Cell& cell = at(update.x, update.y);
                    // UPD contains the new values for the cell, not deltas.
                    cell.humans = update.humans;
                    cell.vampires = update.vampires;
                    cell.werewolves = update.werewolves;
                }
                inferOurSpecies();
                break;

            case Command::End:
            case Command::Bye:
                break;
        }
    }

    std::uint8_t rows() const { return rows_; }
    std::uint8_t columns() const { return columns_; }
    Species ourSpecies() const { return ourSpecies_; }

    bool isInside(std::uint8_t x, std::uint8_t y) const {
        return x < columns_ && y < rows_;
    }

    const Cell& cellAt(std::uint8_t x, std::uint8_t y) const {
        return cells_.at(index(x, y));
    }

    std::uint8_t ourCount(const Cell& cell) const {
        if (ourSpecies_ == Species::Vampires) return cell.vampires;
        if (ourSpecies_ == Species::Werewolves) return cell.werewolves;
        return 0;
    }

    std::uint8_t enemyCount(const Cell& cell) const {
        if (ourSpecies_ == Species::Vampires) return cell.werewolves;
        if (ourSpecies_ == Species::Werewolves) return cell.vampires;
        return 0;
    }

private:
    std::uint8_t rows_{};
    std::uint8_t columns_{};
    std::vector<Cell> cells_;
    std::vector<Position> houses_;
    Position homePosition_{};
    bool hasHomePosition_{false};
    Species ourSpecies_{Species::Unknown};

    std::size_t index(std::uint8_t x, std::uint8_t y) const {
        return static_cast<std::size_t>(y) * columns_ + x;
    }

    Cell& at(std::uint8_t x, std::uint8_t y) {
        return cells_.at(index(x, y));
    }

    void inferOurSpecies() {
        if (!hasHomePosition_ || !isInside(homePosition_.x, homePosition_.y)) {
            return;
        }
        const Cell& home = at(homePosition_.x, homePosition_.y);
        if (home.vampires > 0 && home.werewolves == 0) {
            ourSpecies_ = Species::Vampires;
        } else if (home.werewolves > 0 && home.vampires == 0) {
            ourSpecies_ = Species::Werewolves;
        }
    }
};

// -----------------------------------------------------------------------------
// Temporary protocol-test move
// -----------------------------------------------------------------------------
// This is deliberately NOT an AI strategy and is not NGS. It emits one
// structurally legal adjacent move so that the socket/protocol path can be
// tested end-to-end. Replace it with the selected agent policy before matches.

static std::optional<Move> chooseProtocolSmokeTestMove(const GameState& state) {
    if (state.ourSpecies() == Species::Unknown) {
        return std::nullopt;
    }

    static constexpr std::array<std::pair<int, int>, 8> directions{{
        {-1, -1}, {0, -1}, {1, -1},
        {-1,  0},          {1,  0},
        {-1,  1}, {0,  1}, {1,  1}
    }};

    int bestScore = -1000000000;
    std::optional<Move> bestMove;
    std::optional<Move> firstLegalMove;

    for (int y = 0; y < state.rows(); ++y) {
        for (int x = 0; x < state.columns(); ++x) {
            const auto sourceX = static_cast<std::uint8_t>(x);
            const auto sourceY = static_cast<std::uint8_t>(y);
            const Cell& source = state.cellAt(sourceX, sourceY);
            const std::uint8_t count = state.ourCount(source);
            if (count == 0) {
                continue;
            }

            for (const auto& [dx, dy] : directions) {
                const int targetX = x + dx;
                const int targetY = y + dy;
                if (targetX < 0 || targetY < 0 ||
                    targetX >= state.columns() || targetY >= state.rows()) {
                    continue;
                }

                const auto targetUx = static_cast<std::uint8_t>(targetX);
                const auto targetUy = static_cast<std::uint8_t>(targetY);
                const Cell& target = state.cellAt(targetUx, targetUy);
                const Move candidate{sourceX, sourceY, count, targetUx, targetUy};

                // A single move of one complete group satisfies the structural
                // movement constraints: adjacent destination, positive count,
                // no split-source accounting, and no cell is both source and
                // destination in this turn.
                if (!firstLegalMove.has_value()) {
                    firstLegalMove = candidate;
                }

                int score = 0;
                const int humans = target.humans;
                const int enemies = state.enemyCount(target);

                if (humans > 0) {
                    score = (static_cast<int>(count) >= humans) ? 3000 + humans : -2000;
                } else if (enemies > 0) {
                    if (2 * static_cast<int>(count) >= 3 * enemies) {
                        score = 2500 + enemies;
                    } else if (2 * enemies >= 3 * static_cast<int>(count)) {
                        score = -10000;
                    } else {
                        score = -3000;
                    }
                } else if (state.ourCount(target) > 0) {
                    score = 500; // regroup with a friendly group
                } else {
                    score = 1000; // empty adjacent cell
                }

                if (score > bestScore) {
                    bestScore = score;
                    bestMove = candidate;
                }
            }
        }
    }

    // The game rules require at least one move per ply. If no safe destination
    // exists, use the first structurally legal adjacent move instead of sending
    // an empty MOV. This is a protocol-test fallback, not a strategic choice.
    return bestMove.has_value() ? bestMove : firstLegalMove;
}

// -----------------------------------------------------------------------------
// Game lifecycle: SET -> HUM -> HME -> MAP -> repeated UPD -> MOV; END resets.
// -----------------------------------------------------------------------------

static bool receiveInitialState(ClientSocket& client, GameState& state) {
    state.clear();
    const std::array<Command, 4> expected{
        Command::Set, Command::Hum, Command::Hme, Command::Map
    };

    for (Command expectedCommand : expected) {
        Message message = client.readMessage();
        if (message.command == Command::Bye) return false;
        if (message.command == Command::End) {
            // END closes the current game, even if setup was interrupted.
            state.clear();
            return true;
        }
        if (message.command != expectedCommand) {
            throw std::runtime_error("Unexpected command during setup; expected " +
                                     std::to_string(static_cast<int>(expectedCommand)));
        }
        state.apply(message);
    }
    return true;
}

static void playConnection(ClientSocket& client) {
    client.sendNme("VVW-Cpp-Client");
    std::cerr << "Registered with server as VVW-Cpp-Client\n";

    while (true) {
        GameState state;
        if (!receiveInitialState(client, state)) {
            std::cerr << "Server sent BYE during setup\n";
            return;
        }

        if (state.rows() == 0 || state.columns() == 0) {
            // An END arrived before the full setup. Wait for the next game.
            continue;
        }

        if (state.ourSpecies() == Species::Unknown) {
            throw std::runtime_error("Could not infer our species from HME and MAP");
        }

        std::cerr << "Game initialized: " << static_cast<int>(state.rows())
                  << " rows x " << static_cast<int>(state.columns()) << " columns; species="
                  << (state.ourSpecies() == Species::Vampires ? "vampires" : "werewolves")
                  << "\n";

        while (true) {
            Message message = client.readMessage();

            if (message.command == Command::Bye) {
                std::cerr << "Server sent BYE; closing client\n";
                return;
            }
            if (message.command == Command::End) {
                std::cerr << "Game ended; waiting for the next game\n";
                break;
            }

            if (message.command == Command::Upd) {
                // Start the local handling timer once the complete UPD packet
                // has arrived. The server's authoritative 2-second clock also
                // includes network transit time, which the client cannot know
                // exactly from this side of the TCP connection.
                const auto receivedAt = std::chrono::steady_clock::now();
                state.apply(message);

                const std::optional<Move> move = chooseProtocolSmokeTestMove(state);
                std::vector<Move> moves;
                if (move.has_value()) {
                    moves.push_back(*move);
                }

                client.sendMoves(moves);
                const auto sentAt = std::chrono::steady_clock::now();
                const auto elapsedMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                    sentAt - receivedAt).count();

                std::cerr << "Received UPD (" << message.updates.size() << " cell updates), sent MOV ("
                          << moves.size() << " move(s)); local handling time " << elapsedMs << " ms\n";
                continue;
            }

            // SET/HUM/HME/MAP are only expected during game setup.
            throw std::runtime_error("Unexpected command during game: " +
                                     std::to_string(static_cast<int>(message.command)));
        }
    }
}

} // namespace vvw

static int runClient(int argc, char* argv[]) {
    if (argc < 3) {
        std::cerr << "Usage: " << argv[0] << " <server-ip> <server-port>\n";
        return 2;
    }

    // The assignment launcher appends IP and port at the end of the command line.
    // Reading the final two arguments also tolerates optional arguments before them.
    const std::string ip = argv[argc - 2];
    int parsedPort = 0;
    try {
        std::size_t parsedCharacters = 0;
        parsedPort = std::stoi(argv[argc - 1], &parsedCharacters);
        if (parsedCharacters != std::string(argv[argc - 1]).size()) {
            throw std::invalid_argument("port contains non-numeric characters");
        }
    } catch (...) {
        std::cerr << "Invalid server port: " << argv[argc - 1] << '\n';
        return 2;
    }

    if (parsedPort < 1 || parsedPort > 65535) {
        std::cerr << "Server port must be between 1 and 65535\n";
        return 2;
    }

    vvw::ClientSocket client(ip, static_cast<std::uint16_t>(parsedPort));
    std::cerr << "Connected to " << ip << ':' << parsedPort << '\n';
    vvw::playConnection(client);
    return 0;
}

int main(int argc, char* argv[]) {
#ifndef _WIN32
    // Prevent a disconnected server from terminating the process with SIGPIPE.
    std::signal(SIGPIPE, SIG_IGN);
#endif

    try {
#ifdef _WIN32
        // Keep Winsock initialized for the lifetime of the socket connection.
        vvw::SocketRuntime socketRuntime;
        (void)socketRuntime;
#endif
        return runClient(argc, argv);
    } catch (const std::exception& error) {
        std::cerr << "Client error: " << error.what() << '\n';
        return 1;
    }
}
