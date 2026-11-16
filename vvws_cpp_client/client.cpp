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

#include "vvw/game/decision_engine_factory.hpp"

#include <memory>

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

// Diagnostics go to stderr so they remain visible in PowerShell and do not
// interfere with any protocol bytes sent over the TCP socket.
static void logInfo(const std::string& message) {
    std::cerr << "[INFO] " << message << '\n';
}

static void logTx(const std::string& message) {
    std::cerr << "[TX]   " << message << '\n';
}

static void logRx(const std::string& message) {
    std::cerr << "[RX]   " << message << '\n';
}

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
        logInfo("Winsock initialized (2.2)");
    }
    ~SocketRuntime() {
        WSACleanup();
        logInfo("Winsock cleaned up");
    }
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
        logTx("NME name=\"" + name + "\" (" +
              std::to_string(name.size()) + " byte(s))");
        sendAll(packet);
        logInfo("NME handshake sent");
    }

    Message readMessage() {
        std::array<std::uint8_t, 3> commandBytes{};
        receiveExact(commandBytes.data(), commandBytes.size());

        const std::string command(commandBytes.begin(), commandBytes.end());
        Message message{};

        if (command == "END") {
            message.command = Command::End;
            logRx("END (game finished)");
            return message;
        }
        if (command == "BYE") {
            message.command = Command::Bye;
            logRx("BYE (server is closing the session)");
            return message;
        }

        if (command == "SET") {
            message.command = Command::Set;
            message.rows = readByte();
            message.columns = readByte();
            logRx("SET rows=" + std::to_string(message.rows) +
                  " columns=" + std::to_string(message.columns));
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
            logRx("HUM house_count=" + std::to_string(message.houses.size()));
            return message;
        }

        if (command == "HME") {
            message.command = Command::Hme;
            message.position.x = readByte();
            message.position.y = readByte();
            logRx("HME home_position=(" + std::to_string(message.position.x) +
                  "," + std::to_string(message.position.y) + ")");
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
            logRx(command + " cell_update_count=" +
                  std::to_string(message.updates.size()));
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

        // Log the packet summary only. Avoid printing every move during a match,
        // because excessive console output can consume part of the 2-second budget.
        logTx("MOV move_count=" + std::to_string(moves.size()));
        sendAll(packet);
        logInfo("MOV packet sent");
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
        logInfo("Resolving server address " + ip_ + ":" + service);
        addrinfo* addresses = nullptr;
        const int lookupResult = getaddrinfo(ip_.c_str(), service.c_str(), &hints, &addresses);
        if (lookupResult != 0) {
#ifdef _WIN32
            throw std::runtime_error("getaddrinfo failed: " + std::to_string(lookupResult));
#else
            throw std::runtime_error(std::string("getaddrinfo failed: ") + gai_strerror(lookupResult));
#endif
        }

        logInfo("Address resolution succeeded; attempting TCP connection");
        int errorCode = 0;
        for (addrinfo* address = addresses; address != nullptr; address = address->ai_next) {
            SocketHandle candidate = ::socket(address->ai_family, address->ai_socktype,
                                              address->ai_protocol);
            if (candidate == INVALID_SOCKET_HANDLE) {
                errorCode = lastSocketError();
                continue;
            }

            logInfo("Trying a resolved server address...");
            if (::connect(candidate, address->ai_addr,
                          static_cast<int>(address->ai_addrlen)) == 0) {
                socket_ = candidate;
                logInfo("TCP connection established to " + ip_ + ":" + service);
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
// Generic decision-engine adapter boundary
// -----------------------------------------------------------------------------
// The TCP client knows only the shared game types and DecisionEngine interface.
// It does not include NGS or Expectiminimax headers. Each executable links one
// implementation of makeDecisionEngine() through its own adapter.

static vvw::game::Species toGameSpecies(Species species) {
    switch (species) {
        case Species::Vampires:
            return vvw::game::Species::Vampires;
        case Species::Werewolves:
            return vvw::game::Species::Werewolves;
        case Species::Unknown:
            return vvw::game::Species::Unknown;
    }
    return vvw::game::Species::Unknown;
}

static vvw::game::BoardState toGameBoardState(const GameState& state) {
    if (state.rows() == 0 || state.columns() == 0) {
        throw std::runtime_error("Cannot convert an uninitialized game board");
    }

    vvw::game::BoardState board;
    board.rows = static_cast<int>(state.rows());
    board.columns = static_cast<int>(state.columns());
    board.ourSpecies = toGameSpecies(state.ourSpecies());
    board.cells.resize(
        static_cast<std::size_t>(board.rows) *
        static_cast<std::size_t>(board.columns)
    );

    for (int y = 0; y < board.rows; ++y) {
        for (int x = 0; x < board.columns; ++x) {
            const Cell& source = state.cellAt(
                static_cast<std::uint8_t>(x),
                static_cast<std::uint8_t>(y)
            );

            vvw::game::Cell& destination = board.at(
                vvw::game::Position{x, y}
            );
            destination.humans = source.humans;
            destination.vampires = source.vampires;
            destination.werewolves = source.werewolves;
            destination.isHouse = source.isHouse;
        }
    }

    // The server's MAP/UPD records provide cell values, not the previous
    // player's MOV source/destination footprint. Never infer that footprint
    // from sparse cell changes.
    board.previousPly.known = false;
    return board;
}

static std::vector<Move> toProtocolMoves(
    const vvw::game::TurnPlan& plan,
    const GameState& state
) {
    if (plan.moves.size() > 255) {
        throw std::runtime_error(
            "Decision engine returned more than 255 moves; MOV count is one byte"
        );
    }

    std::vector<Move> moves;
    moves.reserve(plan.moves.size());

    for (std::size_t i = 0; i < plan.moves.size(); ++i) {
        const vvw::game::Move& move = plan.moves[i];

        if (move.count == 0 || move.count > 255) {
            throw std::runtime_error(
                "Decision engine returned a movement count outside MOV wire limits"
            );
        }

        if (move.source.x < 0 || move.source.y < 0 ||
            move.destination.x < 0 || move.destination.y < 0 ||
            move.source.x >= static_cast<int>(state.columns()) ||
            move.destination.x >= static_cast<int>(state.columns()) ||
            move.source.y >= static_cast<int>(state.rows()) ||
            move.destination.y >= static_cast<int>(state.rows())) {
            throw std::runtime_error(
                "Decision engine returned a source or destination outside the board/wire range"
            );
        }

        moves.push_back(Move{
            static_cast<std::uint8_t>(move.source.x),
            static_cast<std::uint8_t>(move.source.y),
            static_cast<std::uint8_t>(move.count),
            static_cast<std::uint8_t>(move.destination.x),
            static_cast<std::uint8_t>(move.destination.y)
        });
    }

    return moves;
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
            logInfo("END received during setup; clearing partial game state");
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
    logInfo("Waiting for initial game setup (SET, HUM, HME, MAP)");

    while (true) {
        GameState state;
        if (!receiveInitialState(client, state)) {
            logInfo("Server sent BYE before game setup completed; exiting");
            return;
        }

        if (state.rows() == 0 || state.columns() == 0) {
            // An END arrived before the full setup. Wait for the next game.
            continue;
        }

        if (state.ourSpecies() == Species::Unknown) {
            throw std::runtime_error("Could not infer our species from HME and MAP");
        }

        logInfo("Game initialized: " + std::to_string(state.rows()) + " rows x " +
                std::to_string(state.columns()) + " columns; species=" +
                (state.ourSpecies() == Species::Vampires ? "vampires" : "werewolves"));
        logInfo("Waiting for game updates (UPD)");

        // One decision engine per game. Its implementation is selected at link
        // time: the NGS executable links the NGS factory, while the Expectiminimax
        // executable links the Expectiminimax factory. The engine is destroyed
        // automatically when this game ends or the connection exits.
        std::unique_ptr<vvw::game::DecisionEngine> engine =
            vvw::game::makeDecisionEngine(toGameSpecies(state.ourSpecies()));

        if (!engine) {
            throw std::runtime_error("Decision-engine factory returned null");
        }

        const vvw::game::BoardState initialBoard = toGameBoardState(state);
        if (!engine->onGameStart(initialBoard)) {
            throw std::runtime_error("Selected decision engine rejected the initial board");
        }

        logInfo("Decision engine initialized; initial MAP board supplied");
        logInfo("Previous opponent MOV footprint is not present in UPD; the client marks it unknown");

        while (true) {
            Message message = client.readMessage();

            if (message.command == Command::Bye) {
                logInfo("Server sent BYE; closing client");
                return;
            }
            if (message.command == Command::End) {
                logInfo("Game ended; waiting for the next game setup");
                break;
            }

            if (message.command == Command::Upd) {
                // Start the local handling timer once the complete UPD packet
                // has arrived. The server's authoritative 2-second clock also
                // includes network transit time, which the client cannot know
                // exactly from this side of the TCP connection.
                const auto receivedAt = std::chrono::steady_clock::now();
                state.apply(message);

                const vvw::game::BoardState board = toGameBoardState(state);
                const vvw::game::TurnPlan plan = engine->chooseMove(board);
                const std::vector<Move> moves = toProtocolMoves(plan, state);

                if (plan.moves.empty()) {
                    logInfo("Decision engine returned no move; sending an empty MOV packet");
                }
                logInfo("Decision engine produced " + std::to_string(moves.size()) +
                        " move(s)");
                client.sendMoves(moves);
                const auto sentAt = std::chrono::steady_clock::now();
                const auto elapsedMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                    sentAt - receivedAt).count();

                std::cerr << "[TIMING] Decision and MOV-send processing=" << elapsedMs
                          << " ms (does not include waiting for or decoding the UPD packet)\n";
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
        std::cerr << "[USAGE] " << argv[0] << " <server-ip> <server-port>\n";
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
        std::cerr << "[ERROR] Invalid server port: " << argv[argc - 1] << '\n';
        return 2;
    }

    if (parsedPort < 1 || parsedPort > 65535) {
        std::cerr << "[ERROR] Server port must be between 1 and 65535\n";
        return 2;
    }

    vvw::logInfo("Starting client for remote endpoint " + ip + ":" +
                 std::to_string(parsedPort));
    vvw::ClientSocket client(ip, static_cast<std::uint16_t>(parsedPort));
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
        vvw::logInfo("Platform: Windows");
        vvw::SocketRuntime socketRuntime;
        (void)socketRuntime;
#else
        vvw::logInfo("Platform: POSIX");
#endif
        return runClient(argc, argv);
    } catch (const std::exception& error) {
        std::cerr << "[ERROR] " << error.what() << '\n';
        return 1;
    }
}
