#include <array>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <exception>
#include <iostream>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#ifdef _WIN32
    #ifndef NOMINMAX
        #define NOMINMAX
    #endif
    #include <winsock2.h>
    #include <ws2tcpip.h>
    using SocketHandle = SOCKET;
    static constexpr SocketHandle kInvalidSocket = INVALID_SOCKET;
#else
    #include <arpa/inet.h>
    #include <cerrno>
    #include <netinet/in.h>
    #include <sys/select.h>
    #include <sys/socket.h>
    #include <sys/types.h>
    #include <sys/wait.h>
    #include <unistd.h>
    using SocketHandle = int;
    static constexpr SocketHandle kInvalidSocket = -1;
#endif

namespace {

void check(bool condition, const std::string& message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

#ifdef _WIN32
class SocketRuntime {
public:
    SocketRuntime() {
        WSADATA data{};
        const int result = WSAStartup(MAKEWORD(2, 2), &data);
        check(result == 0, "WSAStartup failed: " + std::to_string(result));
    }
    ~SocketRuntime() { WSACleanup(); }
    SocketRuntime(const SocketRuntime&) = delete;
    SocketRuntime& operator=(const SocketRuntime&) = delete;
};
#endif

void closeSocket(SocketHandle socketHandle) noexcept {
    if (socketHandle == kInvalidSocket) {
        return;
    }
#ifdef _WIN32
    closesocket(socketHandle);
#else
    ::close(socketHandle);
#endif
}

SocketHandle createListener(std::uint16_t& port) {
    const SocketHandle listener = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    check(listener != kInvalidSocket, "Could not create mock-server socket");

    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    address.sin_port = 0; // Ask the operating system for an available port.

    if (::bind(listener, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) != 0) {
        closeSocket(listener);
        throw std::runtime_error("Could not bind mock-server socket");
    }
    if (::listen(listener, 1) != 0) {
        closeSocket(listener);
        throw std::runtime_error("Could not listen on mock-server socket");
    }

    sockaddr_in boundAddress{};
#ifdef _WIN32
    int addressLength = sizeof(boundAddress);
#else
    socklen_t addressLength = sizeof(boundAddress);
#endif
    if (::getsockname(listener, reinterpret_cast<sockaddr*>(&boundAddress), &addressLength) != 0) {
        closeSocket(listener);
        throw std::runtime_error("Could not read mock-server port");
    }
    port = ntohs(boundAddress.sin_port);
    return listener;
}

SocketHandle acceptWithTimeout(SocketHandle listener) {
    fd_set readSet;
    FD_ZERO(&readSet);
    FD_SET(listener, &readSet);
    timeval timeout{};
    timeout.tv_sec = 10;
    timeout.tv_usec = 0;
#ifdef _WIN32
    const int ready = ::select(0, &readSet, nullptr, nullptr, &timeout);
#else
    const int ready = ::select(listener + 1, &readSet, nullptr, nullptr, &timeout);
#endif
    check(ready > 0, ready == 0 ? "Timed out waiting for client connection"
                               : "select() failed while waiting for client");

    const SocketHandle connection = ::accept(listener, nullptr, nullptr);
    check(connection != kInvalidSocket, "Mock server could not accept client connection");
    return connection;
}

void sendAll(SocketHandle socketHandle, const std::uint8_t* data, std::size_t length) {
    std::size_t sent = 0;
    while (sent < length) {
#ifdef _WIN32
        const int result = ::send(socketHandle,
                                  reinterpret_cast<const char*>(data + sent),
                                  static_cast<int>(length - sent), 0);
#else
        const ssize_t result = ::send(socketHandle, data + sent, length - sent, 0);
#endif
        if (result < 0) {
#ifndef _WIN32
            if (errno == EINTR) {
                continue;
            }
#endif
            throw std::runtime_error("Mock server send failed");
        }
        check(result != 0, "Mock server send returned zero bytes");
        sent += static_cast<std::size_t>(result);
    }
}

void receiveExact(SocketHandle socketHandle, std::uint8_t* data, std::size_t length) {
    std::size_t received = 0;
    while (received < length) {
#ifdef _WIN32
        const int result = ::recv(socketHandle,
                                  reinterpret_cast<char*>(data + received),
                                  static_cast<int>(length - received), 0);
#else
        const ssize_t result = ::recv(socketHandle, data + received, length - received, 0);
#endif
        if (result < 0) {
#ifndef _WIN32
            if (errno == EINTR) {
                continue;
            }
#endif
            throw std::runtime_error("Mock server receive failed");
        }
        check(result != 0, "Client closed before sending a complete packet");
        received += static_cast<std::size_t>(result);
    }
}

void sendFragmented(SocketHandle socketHandle, const std::vector<std::uint8_t>& packet) {
    // Send one byte per socket operation to exercise the client's exact-read loop.
    for (const std::uint8_t byte : packet) {
        sendAll(socketHandle, &byte, 1);
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
}

void expectCommand(const std::array<std::uint8_t, 3>& command, const char* expected) {
    check(command[0] == static_cast<std::uint8_t>(expected[0]) &&
          command[1] == static_cast<std::uint8_t>(expected[1]) &&
          command[2] == static_cast<std::uint8_t>(expected[2]),
          std::string("Expected ") + expected + " command");
}

void runMockServer(SocketHandle listener) {
    const SocketHandle connection = acceptWithTimeout(listener);
    try {
        std::array<std::uint8_t, 4> nmeHeader{};
        receiveExact(connection, nmeHeader.data(), nmeHeader.size());
        expectCommand({nmeHeader[0], nmeHeader[1], nmeHeader[2]}, "NME");
        check(nmeHeader[3] > 0, "Client sent an empty name");
        std::vector<std::uint8_t> name(nmeHeader[3]);
        receiveExact(connection, name.data(), name.size());

        const std::vector<std::vector<std::uint8_t>> setupPackets{
            {'S', 'E', 'T', 5, 5},
            {'H', 'U', 'M', 1, 0, 0},
            {'H', 'M', 'E', 2, 2},
            {'M', 'A', 'P', 1, 2, 2, 0, 5, 0},
            {'U', 'P', 'D', 0}
        };
        for (const auto& packet : setupPackets) {
            sendFragmented(connection, packet);
        }

        std::array<std::uint8_t, 4> movHeader{};
        receiveExact(connection, movHeader.data(), movHeader.size());
        expectCommand({movHeader[0], movHeader[1], movHeader[2]}, "MOV");
        const std::uint8_t moveCount = movHeader[3];
        check(moveCount >= 1, "Client must send at least one move for this test state");

        std::vector<std::uint8_t> moveBytes(static_cast<std::size_t>(moveCount) * 5);
        receiveExact(connection, moveBytes.data(), moveBytes.size());

        const std::uint8_t sourceX = moveBytes[0];
        const std::uint8_t sourceY = moveBytes[1];
        const std::uint8_t creatureCount = moveBytes[2];
        const std::uint8_t targetX = moveBytes[3];
        const std::uint8_t targetY = moveBytes[4];

        check(sourceX == 2 && sourceY == 2, "MOV source must be the home cell (2,2)");
        check(creatureCount == 5, "MOV must move the five creatures in the source group");
        const int dx = static_cast<int>(targetX) - static_cast<int>(sourceX);
        const int dy = static_cast<int>(targetY) - static_cast<int>(sourceY);
        check((dx != 0 || dy != 0) && std::abs(dx) <= 1 && std::abs(dy) <= 1,
              "MOV destination must be an adjacent, different cell");

        sendFragmented(connection, {'E', 'N', 'D'});
        sendFragmented(connection, {'B', 'Y', 'E'});
    } catch (...) {
        closeSocket(connection);
        throw;
    }
    closeSocket(connection);
}

std::string quoteExecutablePath(const std::string& path) {
#ifdef _WIN32
    // CTest supplies a normal filesystem path; quoting handles spaces in it.
    return "\"" + path + "\"";
#else
    std::string quoted = "'";
    for (const char character : path) {
        if (character == '\'') {
            quoted += "'\\''";
        } else {
            quoted += character;
        }
    }
    quoted += "'";
    return quoted;
#endif
}

int runClientProcess(const std::string& executable, std::uint16_t port) {
    const std::string command = quoteExecutablePath(executable) +
                                " 127.0.0.1 " + std::to_string(port);
    const int result = std::system(command.c_str());
    check(result != -1, "Could not launch C++ client process");
#ifdef _WIN32
    return result;
#else
    check(WIFEXITED(result), "C++ client did not exit normally");
    return WEXITSTATUS(result);
#endif
}

} // namespace

int main(int argc, char* argv[]) {
    try {
#ifdef _WIN32
        SocketRuntime socketRuntime;
#endif
        check(argc == 2, "Usage: vvws_cpp_mock_server_test /path/to/vvws_client");

        std::uint16_t port = 0;
        const SocketHandle listener = createListener(port);
        std::exception_ptr serverError;
        std::thread serverThread([&]() {
            try {
                runMockServer(listener);
            } catch (...) {
                serverError = std::current_exception();
            }
        });

        int clientExitCode = -1;
        try {
            clientExitCode = runClientProcess(argv[1], port);
        } catch (...) {
            serverThread.join();
            closeSocket(listener);
            throw;
        }

        serverThread.join();
        closeSocket(listener);
        if (serverError) {
            std::rethrow_exception(serverError);
        }
        check(clientExitCode == 0,
              "C++ client exited with status " + std::to_string(clientExitCode));

        std::cout << "PASS: C++ mock server verified NME, fragmented reads, setup, MOV, END and BYE\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "FAIL: " << error.what() << '\n';
        return 1;
    }
}
