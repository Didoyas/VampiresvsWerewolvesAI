# Windows teammate handoff: current NGS self-play and trace validation

## Purpose

Run the current Windows NGS client in both player slots against the course-provided
Windows game server. The benchmark starts one server process and two concurrent
clients per game, saves both client logs and the server trace, and detects match
completion from the `END` and `BYE` messages received by each client.

For the complete workflow, including Wireshark/Npcap capture and `MOV` analysis,
follow [`README.md`](README.md). This file is the short Windows execution checklist.

## 1. Prerequisites

- Windows x64 with the course server executable and its adjacent `.exe.config`.
- The map XML referenced by the server config.
- A Windows x64 build of `vvws_client.exe`.
- Python 3.10 or newer.
- Git, CMake, and MSYS2 MinGW 64-bit if building the client on this machine.

The runner does not modify the course server, its config, the map, or the game protocol.
Do not start a second server manually. The configured port must be free.

## 2. Build and test the client

From the MSYS2 MinGW 64-bit terminal:

```bash
cd /c/Projects/VampiresvsWereWolvesAI/vvws_cpp_client
cmake -S . -B build/windows-x64 -G "MinGW Makefiles" \
  -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING=ON
cmake --build build/windows-x64 --parallel
ctest --test-dir build/windows-x64 --output-on-failure
```

Do not continue if configuration, compilation, or any CTest test fails.
The expected executable is `vvws_cpp_client/build/windows-x64/vvws_client.exe`.

## 3. Prepare the benchmark config

Open PowerShell at the repository root, then:

```powershell
Set-Location .\tools\self_play_benchmark
Copy-Item .\benchmark_config.example.json .\benchmark_config.json
notepad .\benchmark_config.json
```

For a current-agent-versus-current-agent self-play run, set both `client_a` and
`client_b` to the same current Windows `vvws_client.exe` and the same server IP
and port. Use the real server path, map path, and endpoint from the Windows
server's adjacent `.exe.config`. Keep `PlayersCount=2` and `AutoRestart=False`.

Start with two games and alternating connection order. The matches run
sequentially, with the two client processes concurrent inside each game.

## 4. Run Python tests and the benchmark

```powershell
py -3 -m unittest discover -s .\tests -v
py -3 -u .\benchmark.py --config .\benchmark_config.json
```

The test suite covers the log parsers, client `END`/`BYE` completion detection,
server trace discovery, persistent-server cleanup, and capture-analysis parsing.
The Python tests do not launch the official server.

The runner now:

1. Takes a snapshot of existing `trace_*.txt` files beside the server executable.
2. Starts the server and both clients.
3. Waits until both client logs contain `[RX] END` and `[RX] BYE`, or until a
   client fails or the configured protocol timeout is reached.
4. Allows clients to exit, then gives the server a short shutdown grace. If the
   server remains open after both clients received closure messages, the runner
   terminates the process as cleanup. This is not counted as a game timeout.
5. Finds the trace created or changed during this game, copies it to
   `game_NNN/server_trace.txt`, and parses metrics from the copy. It never moves
   or deletes the source trace.

`server.log` remains the server stdout/stderr capture. It may be empty when the
course server writes diagnostics to its separate `trace_*.txt` file. Check
`server_metrics_source` and `server_trace_found` in the game JSON/CSV; do not
interpret zero parser counts as actual zero server events when the source is
`unavailable`.

## 5. Inspect the results

Each timestamped run directory should contain:

- `manifest.json`, `summary.csv`, and `summary.json`.
- One `game_NNN.json` per match.
- `game_NNN/server.log` (server stdout/stderr).
- `game_NNN/server_trace.txt` (copied server trace, when discovered).
- `game_NNN/player_0.log` and `player_1.log`.

For each game, verify:

- both client logs show initialization and opposite species;
- both player logs contain `[RX] END` and `[RX] BYE`;
- `completed_protocol=true` and `timed_out=false`;
- `server_metrics_source=server_trace_file` for the tested course server;
- expected plies and MOV packet counts appear in the server metrics;
- zero-move, malformed MOV, and rejection counts are inspected rather than
  assumed;
- latency metrics are present and remain within the local 1500 ms target.

The verified protocol does not provide a winner or final board in `END`/`BYE`.
Do not infer a winner from the final player to move, process exit order, or move
counts. A protocol-complete match is not necessarily a strategically successful
match.

## 6. Capture submitted MOV commands (optional, needs administrator setup)

Install Wireshark for Windows with Npcap. Npcap requires machine-level driver
installation and may require an administrator. List capture interfaces:

```powershell
& "C:\Program Files\Wireshark\dumpcap.exe" -D
```

When both clients and the server use `127.0.0.1` on this same machine, select
`Adapter for loopback traffic capture`. Then follow the combined capture steps
in [`README.md`](README.md), using the selected interface number.

The analyzer decodes client-submitted `MOV` messages. A capture alone does not
prove that the server accepted or executed a command. Do not treat captured
submissions as confirmed opponent history without an authoritative acceptance
signal.

## 7. Strategic diagnostic run (optional)

To log the moves selected by the NGS decision engine, use a build that includes
the diagnostic change, then set the environment variable before launching the
benchmark:

```powershell
$env:VVW_NGS_DIAGNOSTICS = "1"
try {
    py -3 -u .\benchmark.py --config .\benchmark_config.json
}
finally {
    Remove-Item Env:VVW_NGS_DIAGNOSTICS -ErrorAction SilentlyContinue
}
```

The client logs include candidate counts and selected movement coordinates.
Use them to determine whether the main groups are stationary because no
candidate is generated, because a candidate is rejected, or because evaluation
ranks a stationary plan above movement. Keep this diagnostic environment
variable off during ordinary competition runs.

## 8. Preserve and return evidence

Keep the entire timestamped benchmark directory unchanged, including raw logs,
server trace copies, and JSON/CSV summaries. Also preserve:

- the commit hash used to build the Windows client;
- complete CTest and Python test output;
- the actual server config values (redact secrets, if any) and map filename;
- the original server trace file if the course server writes it outside the
  benchmark output directory;
- the `.pcapng` and capture-analysis directory if packet capture was enabled.

Do not edit raw evidence files after collection. The winner is unknown unless a
separate authoritative result source is available.
