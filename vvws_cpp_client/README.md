# Vampires vs Werewolves — C++ TCP client

This project contains the C++ TCP client for the Vampires vs Werewolves game.
It currently implements the **communication/protocol layer and a temporary
smoke-test move policy**. It does not yet implement NGS, MCTS, reinforcement
learning, or a competitive game strategy.

The function `chooseProtocolSmokeTestMove()` exists only to verify that the
client can respond to an `UPD` message with a movement. Replace this temporary
policy with the agent's decision-making logic before tournament play.

## 1. Project layout

Keep this folder under the main project directory:

```text
VampiresvsWerewolvesAI/
├── README.md
├── .gitignore
├── vvws_cpp_client/
│   ├── CMakeLists.txt
│   ├── client.cpp
│   ├── README.md
│   ├── .gitignore
│   ├── cmake/
│   │   └── toolchains/
│   │       └── mingw-w64-x86_64.cmake
│   └── tests/
│       ├── mock_server_test.cpp
│       └── mock_server_test.py  # Optional Python-based regression harness
└── game_server/
    ├── Server v3.2.1/
    └── Map Generator/
```

The primary integration test is written in C++ and is run through CTest. The
optional Python mock-server harness provides a second independent check. The C++
client and its C++ test do not depend on Python; Python 3 is needed only to run
the optional Python harness. Python environment configuration is managed
outside this project folder.

The C++ client does not link against `VVSW.Controls.dll` or `VVSW.Model.dll`.
It also does not need `MapGenerator.exe` or the XML maps to compile. Keep the
professor-provided server and map-generator files in their separate folders.

## 2. Prerequisites on macOS

For native macOS development and protocol testing, install:

- Apple Command Line Tools (including Clang).
- CMake.
- Python 3 only if you also want to run the optional Python regression harness.

Install the Command Line Tools if they are not already present:

```bash
xcode-select --install
```

Install CMake with Homebrew:

```bash
brew install cmake
```

Check that the build tools are available:

```bash
clang++ --version
cmake --version
```

Check Python 3 as well only when you plan to run the optional Python harness:

```bash
python3 --version
```

Run all commands below from the `vvws_cpp_client` directory:

```bash
cd /path/to/VampiresvsWerewolvesAI/vvws_cpp_client
```

Replace the example path with the actual location of your project.

## 3. Build and test natively on macOS

Configure the macOS build:

```bash
cmake -S . -B build/macos -DCMAKE_BUILD_TYPE=Release
```

Compile the client:

```bash
cmake --build build/macos --parallel
```

Run the C++ integration test through CTest:

```bash
ctest --test-dir build/macos --output-on-failure
```

The C++ test starts a local mock TCP server, launches the C++ client, sends
protocol messages in deliberately fragmented pieces, and validates the client's
`NME` handshake, setup-message handling, `MOV` response, `END` reset, and `BYE`
shutdown. It also checks that the returned move uses the expected source group
and an adjacent destination. A successful run reports `100% tests passed`.

Optionally, run the independent Python-based regression harness as well (Python
3 required):

```bash
python3 tests/mock_server_test.py ./build/macos/vvws_client
```

The macOS executable (`build/macos/vvws_client`) is for local development and
mock-server testing. It is **not** the Windows competition executable.

## 4. Cross-compile a 64-bit Windows executable from macOS

Install the MinGW-w64 cross-compiler once using Homebrew:

```bash
brew install mingw-w64
```

Confirm that the cross-compiler is on your `PATH`:

```bash
x86_64-w64-mingw32-g++ --version
```

Configure the Windows x86-64 build using the supplied CMake toolchain file:

```bash
cmake -S . -B build/windows-x64 \
  -DCMAKE_TOOLCHAIN_FILE=cmake/toolchains/mingw-w64-x86_64.cmake \
  -DCMAKE_BUILD_TYPE=Release
```

Compile it:

```bash
cmake --build build/windows-x64 --parallel
```

The resulting executable is:

```text
build/windows-x64/vvws_client.exe
```

You can check the binary format from macOS with:

```bash
file build/windows-x64/vvws_client.exe
```

It should identify the output as a 64-bit Windows PE executable. macOS cannot
run this `.exe` directly. Copy it to a Windows machine or Windows VM to test it
against the professor's Windows game server. The build also creates the C++
integration-test executable. On Windows, run the test from the project folder
with:

```powershell
ctest --test-dir build/windows-x64 --output-on-failure
```

Run CTest on Windows; the cross-compiled Windows test executable cannot be run
natively on macOS.

The MinGW build is configured to statically link the GNU C++ runtime. Windows
system libraries, including Winsock, are still provided by Windows.

## 5. Alternative: build natively on Windows with Visual Studio

On a Windows machine with Visual Studio (including the C++ desktop workload)
and CMake installed, open a Developer PowerShell and change to the
`vvws_cpp_client` directory. Configure and build the x64 executable and test:

```powershell
cmake -S . -B build\windows-msvc -A x64
cmake --build build\windows-msvc --config Release
ctest --test-dir build\windows-msvc -C Release --output-on-failure
```

The resulting executable is:

```text
build\windows-msvc\Release\vvws_client.exe
```

## 6. Run the client

The client expects the server IP address and TCP port as its final two command-line
arguments:

```text
vvws_client <server-ip> <server-port>
```

For example, when the game server is listening on the same Windows machine at
port `5555`, open a terminal in the directory containing the executable and run:

```powershell
.\vvws_client.exe 127.0.0.1 5555
```

Use the actual IP address and port supplied by the game server or competition
launcher. Start the server first, and make sure the client can reach the host
and port through the machine's network and firewall configuration.

For a MinGW cross-compiled build, the executable is at
`build/windows-x64/vvws_client.exe` when viewed from the `vvws_cpp_client`
directory.

## 7. Protocol behavior

The client currently handles the following protocol flow:

1. Connect to the server and send `NME` with the client name.
2. Read the setup messages: `SET`, `HUM`, `HME`, and `MAP`.
3. Read each `UPD` message and respond with `MOV`.
4. On `END`, clear the current game state and wait for the next game setup.
5. On `BYE`, close the connection.

Socket reads and writes loop until the complete requested packet has been
transferred. This matters because TCP can split one logical protocol message
across multiple socket operations. On Windows, the client initializes Winsock
and links against `ws2_32`; on macOS, it uses POSIX sockets. No third-party C++
networking library is required.

## 8. Important limitations before tournament play

- The current move-selection function is a temporary smoke-test policy, not a
  competitive strategy. It must be replaced by the team's agent policy.
- The assignment specifies a **2-second response limit** for each `UPD`. Keep
  the final policy within this deadline, including state processing and move
  generation.
- Passing the mock-server test confirms the tested protocol path only. It does
  not establish full compatibility with every server scenario or validate the
  strategic quality and legality of all possible agent moves.
- Test the final Windows executable against the professor-provided server on
  Windows before submission.

## 9. Clean and rebuild

CMake keeps generated files under `build/`, which is ignored by Git. To remove
all local build output and start again from a clean state:

```bash
rm -rf build
```

Then rerun the appropriate configure and build commands from the sections
above.
