# Vampires vs Werewolves benchmark and TCP movement capture

This folder contains the two-client benchmark runner and an optional
Npcap/Wireshark capture workflow. The runner starts the course server and two
client processes, detects protocol completion from each client's `END` and
`BYE` messages, and collects the server trace file written beside the server
executable. The capture wrapper runs **around** the benchmark and does not
modify the game server or the client wire protocol.

The purpose of the capture is to inspect the exact `MOV` commands submitted by
both client instances when the server and both clients are running on the same
Windows machine. This is useful for validating whether the opponent's movement
commands are observable outside the client's own process.

## Important interpretation and limits

The analyzer extracts `MOV` messages sent from a client TCP endpoint to the
server TCP endpoint. These are **submitted movements**, not confirmed movements.
A packet capture by itself cannot prove that the server accepted, validated,
executed, or applied a command. Do not feed these rows into the agent as
`confirmed` opponent movement history.

The labels `connection_1` and `connection_2` are assigned by capture order. They
are not guaranteed to mean vampires/werewolves, baseline/candidate, or server
player slot 0/1. Cross-reference the connection endpoints and the client logs
when assigning an agent identity.

The server's `UPD` records are cell values, not an explicit copy of the previous
player's movement instruction. The analyzer deliberately does not infer a
confirmed source/destination from `UPD` records or board differences.

For the competition server hosted by the professor, capture on the server
machine or an authorized network observation point is needed to see both
players' unicast streams. Capturing on one player's laptop normally sees only
that laptop's traffic. A switched Ethernet/Wi-Fi network or mesh network does
not automatically expose another player's unicast traffic.

## Folder contents

```text
self_play_benchmark/
├── benchmark.py                    # Existing two-client benchmark runner
├── benchmark_config.example.json   # Example config; copy and edit locally
├── capture_and_benchmark.py        # Starts dumpcap, runs benchmark, stops capture
├── analyze_capture.py              # Reassembles TCP streams and decodes MOV frames
├── tests/
│   ├── test_benchmark.py
│   └── test_capture_analysis.py
└── README.md
```

## Requirements

For the combined capture + benchmark workflow:

- Windows 10/11 on the machine where the benchmark server and both clients run.
- Python 3.10 or newer (`py -3 --version`).
- Wireshark for Windows, including Npcap, `dumpcap.exe`, and `tshark.exe`.
- The official course server executable, its adjacent `.exe.config`, and its map.
- Two Windows client builds, each at a separate path.
- Permission to capture traffic on the machine/network. Use this workflow only
  on your own test setup or where the network owner has authorized capture.

The capture scripts use the Python standard library. They do not require
Scapy, pandas, or another Python package. The analyzer uses `tshark`, installed
with Wireshark.

Official references:

- [Wireshark dumpcap manual](https://www.wireshark.org/docs/man-pages/dumpcap.html)
- [Wireshark tshark manual](https://www.wireshark.org/docs/man-pages/tshark.html)
- [Wireshark loopback capture guide](https://wiki.wireshark.org/captureSetup/loopback)

## A. Prepare the existing benchmark

### 1. Keep the server and client executables separate

Build or copy the two Windows client executables to separate paths. For
example, `client_a` can be the baseline and `client_b` can be the candidate.
Do not overwrite one build with the other.

The benchmark starts one server process per game. Do not start the server
manually before running the benchmark. The configured TCP port must be free.

### 2. Create the benchmark config

From this directory in PowerShell:

```powershell
Copy-Item .\benchmark_config.example.json .\benchmark_config.json
notepad .\benchmark_config.json
```

Edit the server path, client paths, game count, and endpoints. The last two
arguments of each client command must be the same server IP and port that the
server uses. Example only:

```json
{
  "server_exe": "C:/VVW/server/VampiresVSWerewolvesGameServer.exe",
  "client_a": ["C:/VVW/clients/baseline/vvws_client.exe", "127.0.0.1", "5555"],
  "client_b": ["C:/VVW/clients/candidate/vvws_client.exe", "127.0.0.1", "5555"],
  "games": 2,
  "output_dir": "./benchmark_results",
  "alternate_connection_order": true,
  "startup_delay_seconds": 2.0,
  "inter_client_delay_seconds": 0.25,
  "server_timeout_seconds": 330,
  "client_exit_grace_seconds": 10,
  "server_exit_grace_seconds": 2,
  "protocol_poll_interval_seconds": 0.1,
  "server_trace_glob": "trace_*.txt"
}
```

Use forward slashes in JSON paths. Keep `PlayersCount=2` and
`AutoRestart=False` in the server's `.exe.config`.

The additional runner settings are:

- `server_timeout_seconds`: maximum time to wait for both client connections
'to receive `END` and `BYE`; it is not a wait-for-server-process-exit timer.
- `client_exit_grace_seconds`: time allowed for each client to exit normally
  after receiving both closure messages.
- `server_exit_grace_seconds`: time allowed for the server process to exit
  after both clients have received closure messages. If it remains open, the
  runner terminates it as post-match cleanup.
- `protocol_poll_interval_seconds`: how frequently the runner checks the
  client logs for closure messages.
- `server_trace_glob`: filename pattern for server trace files, defaulting to
  `trace_*.txt`.
- `server_trace_dir` (optional): directory containing the trace file. When
  omitted, the runner searches beside `server_exe`, which is where the tested
  course server wrote its traces.

### 3. Run the Python tests before capturing

```powershell
py -3 -m unittest discover -s .\tests -v
```

The tests exercise the benchmark log parsers, per-client protocol completion,
server trace discovery, the persistent-server cleanup path, and the TCP
reassembly/MOV decoder. They do not replace a real Windows server smoke test.

## B. Identify the correct capture interface

### 4. List interfaces visible to dumpcap

From PowerShell:

```powershell
& "C:\Program Files\Wireshark\dumpcap.exe" -D
```

If Wireshark was installed in another location, use that path. Example output
will list numbered interfaces, with descriptions such as Ethernet, Wi-Fi, and
`Adapter for loopback traffic capture`. Use the number at the start of the line
as the `--interface` value.

Choose the interface based on the address in `client_a` and `client_b`:

| Client endpoint in benchmark config | Interface to try |
|---|---|
| `127.0.0.1` or `localhost` | `Adapter for loopback traffic capture` |
| The same Windows machine's LAN IPv4 address | First try the physical Ethernet/Wi-Fi interface assigned that address; if no packets appear, test the loopback capture adapter as well |
| A remote server IP | The interface used to reach that remote host, but this captures only traffic visible at this client machine |

For your local benchmark topology, where the server and both clients are all on
the same Windows machine, the loopback adapter is the expected choice when
the clients connect to `127.0.0.1`.

If capture fails with an access/permission error, close PowerShell, reopen it
with **Run as administrator**, and repeat. Do not enable promiscuous capture or
monitor mode unless your network administrator specifically requires it.

## C. Run benchmark and capture together

### 5. Start the combined workflow

Change to this directory, then run the command below using the interface number
identified in step 4:

```powershell
py -3 .\capture_and_benchmark.py `
  --config .\benchmark_config.json `
  --interface 4
```

Replace `4` with your actual interface number. Do not copy the example number
without checking `dumpcap -D` on that machine.

The wrapper performs these operations:

1. Reads the TCP port from the existing `client_a`/`client_b` command arrays.
2. Starts `dumpcap` on the selected interface, filtered to that TCP port.
3. Starts the existing `benchmark.py` with the same config.
4. Waits for the benchmark to finish, then stops `dumpcap` cleanly.
5. Runs `analyze_capture.py` against the saved `.pcapng` file.

The capture wrapper does not edit your config. The benchmark runner also
collects the server's `trace_*.txt` file from beside the server executable,
copies it to each `game_NNN/server_trace.txt`, and parses metrics from that
trace. `server.log` is still preserved as the server's stdout/stderr log.

The runner now considers a match protocol-complete when **both client logs
contain both `[RX] END` and `[RX] BYE`**. It does not wait for the server process
to exit. After client closure it gives the server a short configurable shutdown
grace, then terminates it as cleanup if the course server remains open. This
cleanup is not classified as a match timeout.

The result JSON and CSV include `server_metrics_source`, `server_trace_source`,
per-player `received_end`/`received_bye`, and
`server_terminated_after_protocol`. If no trace or non-empty stdout is found,
`server_metrics_source` is `unavailable`; zero parser counts in that case must
not be interpreted as true zero server events. A trace file is selected only
when it is new or changed during the current game, so a previous game's trace
is not silently reused. The source file is copied, not moved or modified.

To capture more than one interface, repeat the option, for example:

```powershell
py -3 .\capture_and_benchmark.py --config .\benchmark_config.json --interface 4 --interface 6
```

Prefer one known-correct interface for the first run. Capturing duplicate
copies of the same connection on multiple interfaces can complicate analysis.

### Enable NGS movement diagnostics for a strategic run

To record the exact movement selected by each client, set the diagnostic
variable before starting the benchmark. Build the client after applying the
current NGS diagnostic change. The additional `[NGS_DIAG]` fields include the
candidate count and each selected movement as
`source_x,source_y:creature_count->target_x,target_y`.

```powershell
$env:VVW_NGS_DIAGNOSTICS = "1"
try {
    py -3 .\capture_and_benchmark.py --config .\benchmark_config.json --interface 4
}
finally {
    Remove-Item Env:VVW_NGS_DIAGNOSTICS -ErrorAction SilentlyContinue
}
```

Use the actual interface number from `dumpcap -D`. The diagnostics are written
to `player_0.log` and `player_1.log` because the benchmark captures each
client's standard output and standard error. They are disabled when the
variable is absent, so normal competition runs remain quiet.

### 6. Output files

The wrapper places captures under the configured benchmark output directory:

```text
benchmark_results/
├── benchmark_YYYYMMDD_HHMMSS/
│   ├── manifest.json
│   ├── game_001/
│   │   ├── server.log
│   │   ├── server_trace.txt
│   │   ├── player_0.log
│   │   └── player_1.log
│   ├── game_001.json
│   ├── summary.csv
│   └── summary.json
└── captures/
    ├── vvw_capture_YYYYMMDD_HHMMSS.pcapng
    ├── vvw_capture_YYYYMMDD_HHMMSS_dumpcap.log
    └── vvw_capture_YYYYMMDD_HHMMSS_analysis/
        ├── capture_analysis.json
        ├── movements.csv
        └── capture_report.txt
```

`movements.csv` includes one row per five-byte movement record. It preserves the
MOV message grouping using `turn_number` and `move_index`, and includes the
packet frame/time associated with the first byte of each message:

- `turn_number`: the order of the MOV message from that TCP connection, not a
  global server ply number.
- `capture_frame` and `capture_time_epoch`: the packet/frame timestamp associated
  with the segment containing the first byte of that MOV message. Use this to
  order submitted commands from both connections by capture time.
- `move_index`: the movement record within that MOV message.
- `source_x`, `source_y`: source cell.
- `creature_count`: number of creatures included in that submitted movement.
- `target_x`, `target_y`: destination cell.
- `connection`, `tcp_stream`, client/server IP and ports: connection metadata.
- `server_acceptance`: explicitly states that acceptance is not proven.

Keep the `.pcapng` file. It is the raw evidence needed to re-run analysis or
inspect the connection in Wireshark.

## D. Analyze an existing capture manually

Use this when a `.pcapng` was captured separately, such as during a test match
where the benchmark runner did not launch the server.

```powershell
py -3 .\analyze_capture.py `
  --pcap .\benchmark_results\captures\your_capture.pcapng `
  --port 5555 `
  --outdir .\benchmark_results\captures\manual_analysis
```

Replace `5555` with the actual game server TCP port. If `tshark.exe` is not on
PATH or in the default Wireshark installation directory, add:

```powershell
--tshark "C:\Program Files\Wireshark\tshark.exe"
```

To inspect the raw capture visually, open the `.pcapng` in Wireshark and apply
this display filter:

```text
tcp.port == 5555
```

Select a packet for one connection, right-click, and choose **Follow > TCP
Stream**. The analyzer reassembles by TCP sequence numbers, so a `MOV` message
may span multiple TCP packets or multiple messages may share one packet.

## E. Manual live capture without running benchmark.py

For a live match where the server and clients are started separately, open a
PowerShell window on the machine that can see both connections and run:

```powershell
& "C:\Program Files\Wireshark\dumpcap.exe" `
  -i 4 `
  -f "tcp port 5555" `
  -w .\live_match_capture.pcapng
```

Leave capture running while the match takes place. Press **Ctrl+C in that
PowerShell window** to stop and close the capture file. Then decode it:

```powershell
py -3 .\analyze_capture.py `
  --pcap .\live_match_capture.pcapng `
  --port 5555 `
  --outdir .\live_match_analysis
```

Change the interface number and port to the real values. If the server is
hosted on the professor's machine, ask for permission before capturing there.
A client-side capture on your own laptop will not normally expose the other
player's unicast packets.

## F. Troubleshooting

### No MOV records decoded

Check these in order:

1. Did you start capture before the clients connected?
2. Is the interface correct for the IP address used by the clients?
3. Is the port correct? The wrapper reads it from `benchmark_config.json`.
4. Does the capture contain packets? Open it in Wireshark and filter with
   `tcp.port == <port>`.
5. Do you see readable `NME`/`MOV` commands in **Follow TCP Stream**? If traffic
   is encrypted, this decoder cannot inspect the application payload.
6. Is `dumpcap` reporting a capture permission or Npcap driver error in the
   `*_dumpcap.log` file?

### Only one connection appears

Confirm both clients successfully connected. On the local benchmark machine,
check that both client processes are active and that the capture includes the
connection handshake from each. If one client connects over another interface,
select the interface carrying that flow.

### TCP gaps are reported

A gap means the capture did not contain a contiguous sequence range. The
analyzer does not invent missing bytes, and a MOV message split across a gap
may be missed. Capture on the server machine and ensure capture starts before
the clients connect. Retransmissions are de-duplicated by TCP sequence number.

### Capture works locally but not on the competition LAN

That is a visibility/topology issue, not a Python parser issue. Ask the
professor or network administrator whether capture can be made on the server
host, a mirrored switch port, or another authorized observation point. A mesh
network does not by itself make every device's unicast TCP traffic visible to
all other devices.

## G. What this does and does not prove

| Evidence | What can be concluded |
|---|---|
| `MOV` bytes captured in client-to-server direction | The client submitted those source/count/destination records to the server TCP endpoint |
| TCP ACK from server | Bytes reached the receiving TCP stack; this is not proof of game-rule acceptance |
| Later `UPD` packet | The server sent a board update; its sparse cell records are not an explicit movement event |
| Server-side game log explicitly confirming a move | Potential authoritative evidence, depending on what the server log records |
| Final authoritative board with a uniquely attributable transition | May support acceptance/execution analysis, but requires a separate validated state-transition method |

Therefore, this capture workflow is a **diagnostic and data-collection step**.
It is not yet a production integration of confirmed opponent movement history.
Keep the existing history ingestion restricted to verified movement events until
an authoritative acceptance signal is available.

## Existing benchmark metrics

The original runner records protocol termination markers, MOV byte counts from
the server log, per-client decision latency, error lines, process return codes,
and timeouts. The protocol does not expose a verified winner/final board in
`END`/`BYE`, so `winner` and win-rate metrics remain unavailable. The client's
1500 ms local decision target is not the server's full authoritative 2-second
clock.

## Run only the benchmark, without packet capture

The existing command remains available:

```powershell
py -3 .\benchmark.py --config .\benchmark_config.json
```

## Tests

Run from this folder:

```powershell
py -3 -m unittest discover -s .\tests -v
```

The same parser tests can also run on macOS/Linux. The actual server benchmark
and live Npcap capture are Windows workflows.
