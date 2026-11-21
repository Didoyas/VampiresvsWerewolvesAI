# Vampires vs Werewolves — Windows TCP Client Setup and Remote Connection Guide

This guide explains, step by step, how to prepare a Windows computer, install the tools needed to build and test the C++ TCP client, verify the client locally, and then connect it to the professor's **remote** Vampires vs Werewolves game server.

> **Important architecture:** The professor's game server is hosted on a separate remote machine. The Windows computer described in this guide runs only the C++ client. Do **not** start the professor's server executable on this Windows computer for the remote-server test.

> **Current strategy limitation:** The current `client.cpp` still contains the temporary `chooseProtocolSmokeTestMove()` policy. It is suitable for checking the communication path, but it is not NGS and must not be treated as the final competition agent. Use a controlled test session for the connection test until NGS is integrated.

---

## 1. Understand what you need to install

There are two different situations. Choose the one that describes what you are doing.

### Option A — You already have `vvws_client.exe` and only need to run it

For a Windows x64 executable built with the project's MinGW static-runtime settings, the runtime requirements are:

- A supported 64-bit Windows computer (Windows 10 or Windows 11).
- The file `vvws_client.exe`.
- A working network connection to the remote server.
- The remote server IP address and TCP port supplied by the professor or competition launcher.

You do **not** need to install CMake, a C++ compiler, Python, Visual Studio, the professor's server files, or the map generator just to run that prebuilt executable. Windows already supplies the Winsock networking system library used by the client.

If Windows reports that a runtime DLL is missing, use the troubleshooting section at the end. A build made with a different compiler may require that compiler's runtime.

### Option B — You need to build the client and run its tests on Windows

Install the following tools:

| Tool | Why it is needed | Required for just running a prebuilt `.exe`? |
|---|---|---|
| Visual Studio Build Tools with **Desktop development with C++** | Provides the MSVC C++ compiler, linker and Windows SDK | No |
| CMake | Configures the C++ project and generates the build files | No |
| Python 3 | Runs the optional Python mock TCP server test | No |
| Git (optional) | Downloads the project if you are using a Git repository | No |

The Python test uses only Python's standard library. There is no need to install packages with `pip`.

---

## 2. Check that Windows is 64-bit

1. Click the Windows **Start** button.
2. Open **Settings**.
3. Select **System**.
4. Scroll down and open **About**.
5. Find **System type**.
6. Confirm that it says something similar to `64-bit operating system, x64-based processor`.

The client executable built for Windows x64 requires a compatible 64-bit Windows environment.

---

## 3. Install the development and test dependencies (only if building/testing from source)

Skip this section if you already have the executable and only want to connect it to the remote server. In that case, continue at Section 6.

### 3.1 Install Microsoft C++ Build Tools

1. Open the official Visual Studio downloads page:
   <https://visualstudio.microsoft.com/downloads/>
2. Find **Tools for Visual Studio** and select **Build Tools for Visual Studio**. The current Visual Studio downloads page lists the supported Build Tools there.
3. Download and run the installer.
4. When the installer displays the **Workloads** screen, select:
   - **Desktop development with C++**
5. In the installation details, make sure the MSVC C++ x64/x86 build tools and a Windows 10 or Windows 11 SDK are selected. Keep the CMake tools component selected if it is offered.
6. Click **Install** and allow the installation to finish.
7. Restart the computer if the installer asks you to.

Official installation instructions: <https://learn.microsoft.com/en-us/cpp/overview/acquire-msvc>

### 3.2 Install CMake

1. Open the official CMake download page:
   <https://cmake.org/download/>
2. Download the Windows x64 installer (`.msi`) for the current stable release.
3. Run the installer.
4. When asked about the PATH setting, select the option that adds CMake to the PATH for the current user (or all users, if you have administrator permission).
5. Finish the installation.
6. Close any PowerShell windows that were already open. Open a new PowerShell window so it can read the updated PATH.

Check that CMake is installed:

```powershell
cmake --version
```

You should see a CMake version number. If PowerShell says that `cmake` is not recognised, reopen PowerShell or check the PATH option in the CMake installer.

### 3.3 Install Python 3

Python is needed only for the optional Python-based mock-server test in Section 5. It is not needed by `vvws_client.exe` itself.

1. Open the official Python downloads page:
   <https://www.python.org/downloads/windows/>
2. Follow the current Windows installation instructions and install a supported Python 3 release. The Python install manager is also available through the Microsoft Store.
3. Complete the installation.
4. Close PowerShell and open a new PowerShell window.

Check the installation:

```powershell
python --version
py --version
```

At least one of these commands should print a Python 3 version. If `python` works but `py` does not, use `python` in the test command instead of `py -3`.

Official Windows instructions: <https://docs.python.org/3/using/windows.html>

### 3.4 (Optional) Install Git

Git is only needed if you plan to clone or update the source repository from a Git host. It is not needed if you have already received the project folder as a ZIP file.

Official download: <https://git-scm.com/download/win>

---

## 4. Put the project files in a simple folder

For the rest of this guide, we will use this example folder:

```text
C:\VVWS\
└── vvws_cpp_client\
    ├── CMakeLists.txt
    ├── client.cpp
    ├── README.md
    ├── cmake\
    │   └── toolchains\
    │       └── mingw-w64-x86_64.cmake
    └── tests\
        ├── mock_server_test.cpp
        └── mock_server_test.py
```

Your folder may have additional files. That is fine. The important files for a source build are `CMakeLists.txt` and `client.cpp`. The Python mock test is `tests\mock_server_test.py`.

If you received a ZIP file:

1. Open File Explorer.
2. Go to the folder where the ZIP was downloaded.
3. Right-click the ZIP file.
4. Select **Extract All...**.
5. Choose `C:\VVWS\` as the destination, or another folder you can easily find.
6. Click **Extract**.

Do not put the project inside a folder whose name you cannot easily type. If your folder name differs from the example, use your actual folder path in the commands below.

---

## 5. Build the Windows client and test it with a local mock server

This is a local protocol test. It checks that the Windows executable can start, open a TCP connection, send `NME`, read fragmented server packets, process the game setup, send `MOV`, and handle `END` and `BYE`.

**This test does not connect to the professor's remote server.** It starts a temporary mock server on your own Windows computer using `127.0.0.1` and an automatically allocated port. Run this test before using the remote server.

### 5.1 Open the correct PowerShell window

1. Click **Start**.
2. Search for **Developer PowerShell for Visual Studio**.
3. Open the Developer PowerShell matching the Build Tools/Visual Studio installation you installed.

This window loads the compiler environment required by the MSVC build tools.

### 5.2 Move into the C++ client project folder

Enter this command (change the path if you extracted the project somewhere else):

```powershell
cd C:\VVWS\vvws_cpp_client
```

Press Enter.

Check that you are in the correct folder:

```powershell
dir
```

You should see `CMakeLists.txt` and `client.cpp` in the list.

### 5.3 Configure a 64-bit Windows build

Run:

```powershell
cmake -S . -B build\windows-msvc -A x64
```

Wait for CMake to finish. It should finish with a message indicating that build files were generated successfully.

If CMake reports that it cannot find a C++ compiler, close the window and reopen **Developer PowerShell for Visual Studio**. If the error remains, return to the Visual Studio Installer and confirm that **Desktop development with C++**, MSVC build tools, and a Windows SDK are installed.

### 5.4 Compile the client and C++ test program

Run:

```powershell
cmake --build build\windows-msvc --config Release
```

Wait for the build to complete. The client executable should be created here:

```text
build\windows-msvc\Release\vvws_client.exe
```

The C++ mock-server test executable is also built when the project's CMake configuration includes the `cpp_client_protocol` test:

```text
build\windows-msvc\Release\vvws_cpp_mock_server_test.exe
```

### 5.5 Run the C++ mock-server test

Run:

```powershell
ctest --test-dir build\windows-msvc -C Release --output-on-failure
```

A successful result should include:

```text
100% tests passed, 0 tests failed out of 1
```

If CTest reports that no tests were found, check that the version of `CMakeLists.txt` in your project includes the `cpp_client_protocol` test and that you configured the build again after updating it.

### 5.6 Run the independent Python mock-server test (optional)

The Python test is an additional independent check. It starts a local mock server, launches the Windows executable, and prints the client's diagnostics.

From the `vvws_cpp_client` project folder, run:

```powershell
py -3 tests\mock_server_test.py .\build\windows-msvc\Release\vvws_client.exe
```

If the `py` command is not available but `python` works, run:

```powershell
python tests\mock_server_test.py .\build\windows-msvc\Release\vvws_client.exe
```

A successful run should end with:

```text
PASS: NME, fragmented packet reads, setup, MOV, END and BYE
```

The executable should also print diagnostic lines such as `[INFO]`, `[TX]`, and `[RX]` while the mock test runs.

**Do not confuse this local test with the remote test.** The local mock test uses `127.0.0.1`. The remote test in the next section uses the IP address and port supplied by the professor.

---

## 6. Prepare for the remote-server connection test

Before continuing, make sure you have the following two values from the professor or competition launcher:

```text
Remote server IP address: <PROFESSOR_SERVER_IP>
Remote server TCP port:   <PROFESSOR_SERVER_PORT>
```

These are placeholders, not real connection values. Use the actual IP and port supplied for your test session.

Do not use `127.0.0.1` here. `127.0.0.1` means “this same Windows computer,” not the professor's remote machine.

### 6.1 Confirm that the client executable exists

If you built the client in Section 5, use this location:

```text
C:\VVWS\vvws_cpp_client\build\windows-msvc\Release\vvws_client.exe
```

If you were given a prebuilt Windows executable, you can place it in a simple folder such as:

```text
C:\VVWS\vvws_client.exe
```

You do not need to copy the professor's server executable, server DLLs, map generator, or XML maps to the Windows client computer for a remote-server connection test.

### 6.2 Check that Windows can reach the remote TCP port

Open a normal PowerShell window. Set the actual IP address and port in these two variables. For example, replace the example values below:

```powershell
$ServerIP = "203.0.113.10"
$ServerPort = 5555
```

Now run:

```powershell
Test-NetConnection -ComputerName $ServerIP -Port $ServerPort -InformationLevel Detailed
```

Find this line in the output:

```text
TcpTestSucceeded : True
```

What the result means:

- `True`: Windows was able to establish a TCP connection to that remote IP address and port at the time of the check.
- `False`: Windows could not establish the TCP connection. Recheck the IP and port, the server's listening state, network/VPN requirements, and firewall rules.

**Important:** `Test-NetConnection` checks TCP reachability only. It does not send the game's `NME` command and does not verify the game protocol. It briefly opens a TCP connection and closes it. On a live competition server, that may appear as an incomplete client connection in the server logs or consume a connection slot. Ask the server operator whether this check is permitted during the session. If not, skip it and test by launching the actual client at the agreed time.

---

## 7. Connect the Windows client to the professor's remote server

Do this only when the professor says the remote server is running and ready to accept your client. If the server accepts players only during a scheduled match, wait until that test window.

### 7.1 Open PowerShell in the folder containing the executable

For a client built in Section 5, run:

```powershell
cd C:\VVWS\vvws_cpp_client\build\windows-msvc\Release
```

For a prebuilt executable copied to `C:\VVWS`, run:

```powershell
cd C:\VVWS
```

Check that the executable is present:

```powershell
dir vvws_client.exe
```

### 7.2 Set the remote server details

Enter the actual remote IP and port supplied by the professor. For example only:

```powershell
$ServerIP = "203.0.113.10"
$ServerPort = 5555
```

Replace both values. Do not type the angle-bracket placeholders.

### 7.3 Launch the client

Run:

```powershell
.\vvws_client.exe $ServerIP $ServerPort
```

The client reads the final two command-line arguments as the server IP and port. This is also compatible with the competition launcher convention of appending the IP and port after any other client arguments.

**Keep this PowerShell window open.** The client is a persistent TCP client and should remain running while the server sends game messages. Do not start multiple copies for the same player/session.

### 7.4 Read the diagnostic messages

The client writes diagnostic messages to the PowerShell console. A successful connection should show messages similar to the following (the exact values will vary):

```text
[INFO] Platform: Windows
[INFO] Winsock initialized (2.2)
[INFO] Starting client for remote endpoint <IP>:<PORT>
[INFO] Resolving server address <IP>:<PORT>
[INFO] Address resolution succeeded; attempting TCP connection
[INFO] Trying a resolved server address...
[INFO] TCP connection established to <IP>:<PORT>
[TX]   NME name="VVW-Cpp-Client" (14 byte(s))
[INFO] NME handshake sent
[INFO] Waiting for initial game setup (SET, HUM, HME, MAP)
[RX]   SET rows=<...> columns=<...>
[RX]   HUM house_count=<...>
[RX]   HME home_position=(<x>,<y>)
[RX]   MAP cell_update_count=<...>
[INFO] Game initialized: <rows> rows x <columns> columns; species=<vampires or werewolves>
[INFO] Waiting for game updates (UPD)
```

When the server sends an update during the game, you should see messages similar to:

```text
[RX]   UPD cell_update_count=<...>
[TX]   MOV move_count=<...>
[INFO] MOV packet sent
[TIMING] Decision and MOV-send processing=<...> ms (...)
```

When the game/session ends, the client may report:

```text
[RX]   END (game finished)
[RX]   BYE (server is closing the session)
```

The client may receive `END` and then wait for another game setup on the same connection. It exits when it receives `BYE`.

### 7.5 What counts as a successful remote connection test?

Treat the test as successful only when you observe the relevant stages:

1. `[INFO] TCP connection established ...` — the TCP connection to the remote endpoint succeeded.
2. `[TX] NME ...` and `[INFO] NME handshake sent` — the client sent its game-protocol identification packet.
3. `[RX] SET`, `[RX] HUM`, `[RX] HME`, and `[RX] MAP` — the server sent the initial game setup and the client parsed it.
4. `[RX] UPD` followed by `[TX] MOV` — the client and server exchanged a game-turn update and response.

A `TcpTestSucceeded : True` result alone is not enough. It proves only that a TCP connection could be opened to that port. Seeing the setup packets proves the application is speaking the expected protocol. Seeing `UPD` followed by `MOV` is the stronger end-to-end test.

If the server accepts the TCP connection but does not immediately send `SET`, the server may be waiting for a match to start or for another player. Confirm the server's expected match workflow with the professor rather than assuming the client has failed.

---

## 8. Troubleshooting

| Symptom | What it means / what to do |
|---|---|
| `vvws_client.exe` is not found | Use `cd` to enter the folder containing the executable. Run `dir` and check the filename. |
| Windows says the app cannot run on this PC | Confirm that you copied the Windows x64 `.exe`, not the macOS executable. |
| A DLL is missing | Identify which compiler produced the executable. A MinGW build with the project's static C++ runtime settings should not need MinGW C++ runtime DLLs. An MSVC build may require the Microsoft Visual C++ x64 Redistributable. Install runtime packages only from Microsoft's official site. |
| `TcpTestSucceeded : False` | Verify the IP and port with the professor. Confirm that the remote server is listening and that the network/VPN/firewall permits the connection. |
| `[ERROR] Could not connect ...` | The client could not establish TCP. Check the IP, port, server availability, VPN/network route, and firewalls. Record the numeric socket error for diagnosis. |
| `TCP connection established`, but no `SET` arrives | The TCP endpoint accepted the connection, but the application has not sent the game setup. The server may be waiting for a match/player, or the endpoint may not be the game protocol service. Check with the server operator. |
| `Unknown server command` | The endpoint sent a command the current client does not recognise. Verify that the IP and port belong to the correct game server and that the protocol version matches the assignment. |
| `Could not infer our species from HME and MAP` | The initial state did not provide a recognisable player population at the supplied home position. Save the logs and check the setup messages and server state. |
| Client sends `MOV` but server rejects it | TCP communication worked, but the move or game-state interpretation may be incompatible with the server's rules. This requires protocol/rules debugging; a successful TCP connection does not prove move legality. |
| Client exits with `Server closed the TCP connection` | The remote side closed the connection. Check whether the match ended, the server disconnected the client, or the server rejected the session. |

Do not disable Windows Defender Firewall entirely. If a firewall rule is suspected, ask the network or server administrator which outbound destination/port must be permitted.

---

## 9. Important limitations and safety checks before competition

- The remote-server test verifies communication; it does not measure NGS performance or competitive strength.
- The current `chooseProtocolSmokeTestMove()` policy is temporary. It is not NGS. Replace it with the team's NGS decision module before an actual competition match.
- The challenge requires a `MOV` response within two seconds of an `UPD`. The current `[TIMING]` diagnostic begins after the complete `UPD` packet has been read, so it is not a complete measurement of the server's end-to-end two-second clock.
- Keep logs from a controlled test session. Record the executable build used, remote IP/port (in a private team log), the time of the test, and whether the test reached `SET/HUM/HME/MAP` and `UPD/MOV`.
- Do not publish the professor's remote server address or port in a public repository unless the professor explicitly permits it.
- Do not run a mock-server test and a real match client using the same executable at the same time unless separate test sessions are intended.

---

## 10. Quick command reference

### Check installed tools (source-build setup)

```powershell
cmake --version
python --version
py --version
```

### Build on Windows

Run from `C:\VVWS\vvws_cpp_client` in Developer PowerShell:

```powershell
cmake -S . -B build\windows-msvc -A x64
cmake --build build\windows-msvc --config Release
```

### Run the C++ mock-server test

```powershell
ctest --test-dir build\windows-msvc -C Release --output-on-failure
```

### Run the Python mock-server test

```powershell
py -3 tests\mock_server_test.py .\build\windows-msvc\Release\vvws_client.exe
```

### Test remote TCP reachability

```powershell
$ServerIP = "<PROFESSOR_SERVER_IP>"
$ServerPort = <PROFESSOR_SERVER_PORT>
Test-NetConnection -ComputerName $ServerIP -Port $ServerPort -InformationLevel Detailed
```

### Launch the client against the remote server

Run from the folder containing `vvws_client.exe`:

```powershell
.\vvws_client.exe $ServerIP $ServerPort
```

Replace the IP and port placeholders with the actual values supplied by the professor.
