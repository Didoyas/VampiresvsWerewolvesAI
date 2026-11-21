# Windows teammate handoff: run the NGS self-play benchmark

## Purpose

Run the baseline NGS client against the horizon-aware NGS client using the
course-provided Windows game-server executable on the Windows computer where
the server and map are available. The benchmark runner launches and stops one
server process for each game, starts both clients, and saves logs and metrics.

This is intended to run locally on Windows. It does not require a remote
server, a Mac connection, or access to a hosted official server.

## What is included in this benchmark bundle

- `benchmark.py`: Windows match runner and log parser.
- `benchmark_config.example.json`: example configuration to copy and edit.
- `tests/test_benchmark.py`: parser unit tests.
- `README.md`: runner reference and metric definitions.

## What the Windows machine must already have

The benchmark ZIP does **not** contain these machine-specific/licensed files.
The teammate running the benchmark must have:

1. `VampiresVSWerewolvesGameServer.exe` and its adjacent
   `VampiresVSWerewolvesGameServer.exe.config` file.
2. The map XML file referenced by the server config. Its configured path must
   exist on this Windows machine.
3. Two Windows client executables built from the project:
   - baseline revision, before horizon-aware evaluation;
   - candidate revision, including horizon-aware evaluation.
4. Python 3.10 or newer.

Do not use macOS client binaries on Windows. Build each client for Windows x64.
Keep the two `.exe` files at separate paths so one build does not overwrite the
other.

## 1. Check the server configuration

Open the `.exe.config` file beside the server executable. The runner expects
these settings:

```xml
<add key="ServerIP" value="127.0.0.1"/>
<add key="ServerPort" value="5555"/>
<add key="PlayersCount" value="2"/>
<add key="Map" value="C:\path\to\map.xml"/>
<add key="MaxGameDuration" value="00:05:00"/>
<add key="MaxPlayDuration" value="00:00:02"/>
<add key="MaxPlays" value="200"/>
<add key="AutoRestart" value="False"/>
```

The IP and port above are examples from the previously verified local setup.
Use the actual values in the local server config. The `Map` path must resolve
on this Windows computer. Keep `PlayersCount=2` and `AutoRestart=False` because
the runner starts a fresh server process for each match. Do not start the server
manually before running the benchmark, because the runner starts it itself and
needs its configured port to be free.

The runner reads the server config but does not edit it.

## 2. Extract the benchmark bundle

For example, extract the ZIP to:

```text
C:\VVW\vvw_self_play_benchmark\
```

Open PowerShell and change into that directory:

```powershell
Set-Location C:\VVW\vvw_self_play_benchmark
```

Check Python:

```powershell
py -3 --version
```

The version must be Python 3.10 or newer.

## 3. Run the included parser tests

```powershell
py -3 -m unittest discover -s .\tests -v
```

These tests check log parsing only. Passing them does not yet prove that the
server and clients can complete a match.

## 4. Create the local benchmark configuration

Copy the example:

```powershell
Copy-Item .\benchmark_config.example.json .\benchmark_config.json
notepad .\benchmark_config.json
```

Replace the paths with the real Windows paths. Use forward slashes in the JSON
paths to avoid escaping backslashes. Example only:

```json
{
  "server_exe": "C:/VVW/server/VampiresVSWerewolvesGameServer.exe",
  "client_a": [
    "C:/VVW/clients/baseline/vvws_client.exe",
    "127.0.0.1",
    "5555"
  ],
  "client_b": [
    "C:/VVW/clients/horizon/vvws_client.exe",
    "127.0.0.1",
    "5555"
  ],
  "games": 2,
  "output_dir": "./benchmark_results",
  "alternate_connection_order": true,
  "startup_delay_seconds": 2.0,
  "inter_client_delay_seconds": 0.25,
  "server_timeout_seconds": 330,
  "client_exit_grace_seconds": 10
}
```

Important:

- `client_a` must point to the baseline `.exe`.
- `client_b` must point to the candidate `.exe`.
- The last two values in each client command must match `ServerIP` and
  `ServerPort` in the server config exactly.
- Start with `games: 2`. This runs one game with A connecting first and a second
  game with B connecting first.

The example assumes the client accepts the server IP and port as its final two
command-line arguments, which is how the project client is currently written.

## 5. Run the two-game smoke test

Make sure no other server process is already using the configured port. Then
run:

```powershell
py -3 .\benchmark.py --config .\benchmark_config.json
```

The runner starts the server, launches both clients, waits for the game to end,
collects the logs, and shuts down any remaining processes before moving to the
next game.

## 6. Check the results

A timestamped directory is created under `benchmark_results`. It contains:

- `summary.csv` and `summary.json`: combined metrics for the run.
- `game_001.json`, etc.: one structured record per game.
- `game_001/server.log`: server trace.
- `game_001/player_0.log` and `player_1.log`: client logs.
- `manifest.json`: configuration and server settings used for the run.

For the first smoke test, check:

- both client logs show successful connection and game initialization;
- each client identifies its species (`vampires` or `werewolves`);
- the server log shows two player connections and game termination;
- `completed_protocol` is true;
- there are no zero-move packets or server rejection markers;
- the client logs contain `[TIMING]` measurements if latency metrics are
  expected. The latency parser relies on the client's `[TIMING]` log line.

Do not interpret an absent timing metric as zero latency. It may mean the
client build does not emit the expected timing log line.

## 7. Important limitation: winner and win rate

The verified server protocol sends `END`/`BYE`, but it does not expose the
winner or a final board in those messages. The server trace logs MOV packet
byte counts, not the contents of each player's MOV. Therefore this version of
the runner records protocol completion, move validity indicators, and local
latency, but deliberately leaves the winner unknown and does not calculate win
rate. Do not infer the winner from the last player to move.

This first run is an operational smoke test and baseline diagnostics run, not
proof that the horizon-aware evaluation is strategically stronger.

## 8. Send the results back

After the two-game run, send the complete timestamped output directory under
`benchmark_results` back to the project owner. Keep all three logs per game,
plus the CSV, JSON, and manifest files. Do not send only the CSV, because the
logs are needed to diagnose failures.

Once the two-game smoke test succeeds, change `games` to a larger number (for
example, 10 or 20) and run again to collect a more useful operational sample.
