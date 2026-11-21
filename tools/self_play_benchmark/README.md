# Vampires vs Werewolves self-play benchmark runner (v1)

This package runs repeatable two-client matches against the official Windows
server and records protocol, completion, and decision-latency diagnostics.
It uses only the Python standard library.

## Important result limitation

The verified TCP protocol ends with `END`/`BYE` but does not provide the winner
or a final board. The server trace records MOV packet byte counts, not MOV
contents. Therefore v1 deliberately leaves `winner` as `null` and does not
calculate win rate. Do not infer the winner from the last player to move.
A trustworthy winner signal is required before win-rate statistics can be
added.

## Requirements

- Run the match runner on Windows, because the verified server is a .NET
  Framework 4.5 executable.
- Python 3.10 or newer.
- A server executable with its sidecar `.exe.config` file.
- The map file referenced by the server config.
- Two client executables, normally one built from the baseline revision and
  one from the candidate revision.

## Prepare two client builds

Build the baseline and candidate from separate Git worktrees or separate clean
checkouts. Do not overwrite one executable with the other. Copy both executable
paths into the JSON config below.

The client command arrays must end with the server IP and port because the
client reads those as its final two arguments.

## Configure

Copy `benchmark_config.example.json` to `benchmark_config.json` and replace:

- `server_exe`: absolute path to `VampiresVSWerewolvesGameServer.exe`.
- `client_a`: baseline client executable and endpoint arguments.
- `client_b`: candidate client executable and endpoint arguments.
- `games`: number of matches to run.
- `output_dir`: where logs and summaries should be written.

The runner reads the sidecar server config and validates `PlayersCount=2`,
`AutoRestart=False`, the configured IP/port, and the map path. It does not edit
the server config.

The default startup delay is 2 seconds, matching the observed server trace in
which the server began waiting for two connections before the first NME
arrived. Increase it if the machine starts the server more slowly.

## Run

From this directory:

```powershell
python .\benchmark.py --config .\benchmark_config.json
```

Output structure:

```text
benchmark_results/
└── benchmark_YYYYMMDD_HHMMSS/
    ├── manifest.json
    ├── game_001/
    │   ├── server.log
    │   ├── player_0.log
    │   └── player_1.log
    ├── game_001.json
    ├── summary.csv
    └── summary.json
```

Connection order is alternated by game number to reduce fixed player-slot
bias. The JSON record shows which agent occupied each slot and each client's
inferred species.

## Recorded metrics

- Maximum observed ply number and number of MOV packets.
- Total MOV bytes, zero-byte MOV packets, malformed MOV packet sizes, and
  maximum number of movements in a single MOV.
- Server END/BYE termination markers and server rejection/cheating markers.
- Per-client decision count, average and maximum local decision latency.
- Number of decisions over the client's 1500 ms local target.
- Client error lines, process return codes, server process return code, and
  timeouts.

The 1500 ms value is a client-side target, not an authoritative measurement of
the server's full 2-second clock. The client's timing log excludes waiting for
and decoding the UPD packet.

## Tests

Parser tests run on macOS/Linux or Windows:

```bash
python -m unittest discover -s tests -v
```

The actual match runner is Windows-only and still requires a real server/client
smoke test on the Windows machine. The included unit tests do not claim to
validate server startup or gameplay.
