#!/usr/bin/env python3
"""Run repeatable two-agent matches against the official VvW Windows server.

This first version records execution, protocol and latency diagnostics. It does
not invent a winner: the verified server protocol sends END/BYE without a final
board or explicit winner field.
"""

from __future__ import annotations

import argparse
import csv
import json
import math
import os
import shutil
import statistics
import subprocess
import sys
import time
import xml.etree.ElementTree as ET
from dataclasses import asdict, dataclass
from datetime import datetime, timezone
from pathlib import Path
from typing import Any


@dataclass
class ClientMetrics:
    label: str
    species: str
    timings_ms: list[float]
    error_lines: int
    return_code: int | None
    connected_and_initialized: bool
    received_end: bool
    received_bye: bool

    @property
    def decision_count(self) -> int:
        return len(self.timings_ms)

    @property
    def mean_ms(self) -> float | None:
        return statistics.fmean(self.timings_ms) if self.timings_ms else None

    @property
    def max_ms(self) -> float | None:
        return max(self.timings_ms) if self.timings_ms else None

    @property
    def local_budget_overruns(self) -> int:
        # The client uses a 1500 ms local target. This is not the server's
        # authoritative 2-second deadline measurement.
        return sum(value > 1500.0 for value in self.timings_ms)


@dataclass
class ServerMetrics:
    plies: int
    mov_packets: int
    total_mov_bytes: int
    zero_move_packets: int
    malformed_mov_packets: int
    max_moves_in_one_packet: int
    end_bye_messages: int
    cheat_or_rejection_markers: int


def read_server_settings(server_exe: Path) -> dict[str, str]:
    config_path = Path(str(server_exe) + ".config")
    if not config_path.is_file():
        raise FileNotFoundError(f"Server config not found: {config_path}")

    root = ET.parse(config_path).getroot()
    settings: dict[str, str] = {}
    for node in root.findall(".//appSettings/add"):
        key = node.get("key")
        value = node.get("value")
        if key and value is not None:
            settings[key] = value
    return settings


def validate_config(config: dict[str, Any]) -> dict[str, str]:
    required = ("server_exe", "client_a", "client_b", "games", "output_dir")
    missing = [key for key in required if key not in config]
    if missing:
        raise ValueError("Missing config keys: " + ", ".join(missing))

    if not isinstance(config["client_a"], list) or not config["client_a"]:
        raise ValueError("client_a must be a non-empty command array")
    if not isinstance(config["client_b"], list) or not config["client_b"]:
        raise ValueError("client_b must be a non-empty command array")
    if int(config["games"]) < 1:
        raise ValueError("games must be at least 1")

    server_exe = Path(config["server_exe"]).expanduser().resolve()
    if not server_exe.is_file():
        raise FileNotFoundError(f"Server executable not found: {server_exe}")

    settings = read_server_settings(server_exe)
    if settings.get("PlayersCount") != "2":
        raise ValueError("Server config must set PlayersCount=2")
    if settings.get("AutoRestart", "False").lower() == "true":
        raise ValueError("Set AutoRestart=False; this runner starts one server per game")
    if not settings.get("ServerIP") or not settings.get("ServerPort"):
        raise ValueError("ServerIP and ServerPort must be present in the server config")

    map_path = Path(settings.get("Map", "")).expanduser()
    if not map_path.is_file():
        raise FileNotFoundError(f"Map from server config was not found: {map_path}")

    port = str(settings["ServerPort"])
    ip = settings["ServerIP"]
    for label in ("client_a", "client_b"):
        command = config[label]
        if len(command) < 3:
            raise ValueError(f"{label} must end with <server-ip> <server-port>")
        if str(command[-2]) != ip or str(command[-1]) != port:
            raise ValueError(
                f"{label} must end with the server config endpoint {ip} {port}"
            )

    return settings


def _extract_integer_after(line: str, marker: str, suffix: str | None = None) -> int | None:
    start = line.find(marker)
    if start < 0:
        return None
    start += len(marker)
    end = line.find(suffix, start) if suffix else -1
    token = line[start:end if end >= 0 else None].strip()
    try:
        return int(token)
    except ValueError:
        return None


def parse_server_log(path: Path) -> ServerMetrics:
    plies = 0
    mov_packets = 0
    total_mov_bytes = 0
    zero_move_packets = 0
    malformed_mov_packets = 0
    max_moves = 0
    end_bye_messages = 0
    rejection_markers = 0

    with path.open("r", encoding="utf-8", errors="replace") as stream:
        for line in stream:
            if "Coup n°" in line:
                value = _extract_integer_after(line, "Coup n°")
                if value is not None:
                    plies = max(plies, value)

            marker = "MOV: en attente de "
            if marker in line:
                byte_count = _extract_integer_after(line, marker, " octets")
                if byte_count is not None:
                    mov_packets += 1
                    total_mov_bytes += byte_count
                    if byte_count == 0:
                        zero_move_packets += 1
                    elif byte_count % 5 != 0:
                        malformed_mov_packets += 1
                    else:
                        max_moves = max(max_moves, byte_count // 5)

            if "Envoi de END et BYE" in line:
                end_bye_messages += 1

            lowered = line.casefold()
            if (
                "triché" in lowered
                or "triche" in lowered
                or "cheat" in lowered
                or "value was either too large or too small" in lowered
            ):
                rejection_markers += 1

    return ServerMetrics(
        plies=plies,
        mov_packets=mov_packets,
        total_mov_bytes=total_mov_bytes,
        zero_move_packets=zero_move_packets,
        malformed_mov_packets=malformed_mov_packets,
        max_moves_in_one_packet=max_moves,
        end_bye_messages=end_bye_messages,
        cheat_or_rejection_markers=rejection_markers,
    )


def client_protocol_state(path: Path) -> tuple[bool, bool]:
    """Return whether the client log contains both server closure messages.

    The current client writes explicit [RX] END and [RX] BYE markers. These
    per-connection markers are the reliable completion signal even when the
    server process intentionally remains alive after the match.
    """
    if not path.is_file():
        return False, False

    received_end = False
    received_bye = False
    with path.open("r", encoding="utf-8", errors="replace") as stream:
        for line in stream:
            if "[RX]" not in line:
                continue
            if "END" in line:
                received_end = True
            if "BYE" in line:
                received_bye = True
            if received_end and received_bye:
                break
    return received_end, received_bye


def parse_client_log(path: Path, label: str, return_code: int | None) -> ClientMetrics:
    timings: list[float] = []
    species = "unknown"
    error_lines = 0
    connected_and_initialized = False
    timing_prefix = "[TIMING] Decision and MOV-send processing="

    if path.is_file():
        with path.open("r", encoding="utf-8", errors="replace") as stream:
            for line in stream:
                if "[ERROR]" in line:
                    error_lines += 1

                if "Game initialized:" in line:
                    # The client emits this only after a successful TCP
                    # connection and completion of the initial SET/HUM/HME/MAP.
                    connected_and_initialized = True

                if "Game initialized:" in line and "species=" in line:
                    if "species=vampires" in line:
                        species = "vampires"
                    elif "species=werewolves" in line:
                        species = "werewolves"

                if timing_prefix in line:
                    value = _extract_integer_after(line, timing_prefix, " ms")
                    if value is None:
                        # Preserve non-integer timing values if a future client
                        # version prints fractional milliseconds.
                        start = line.find(timing_prefix) + len(timing_prefix)
                        end = line.find(" ms", start)
                        try:
                            timings.append(float(line[start:end].strip()))
                        except (ValueError, TypeError):
                            pass
                    else:
                        timings.append(float(value))

    received_end, received_bye = client_protocol_state(path)
    return ClientMetrics(
        label=label,
        species=species,
        timings_ms=timings,
        error_lines=error_lines,
        return_code=return_code,
        connected_and_initialized=connected_and_initialized,
        received_end=received_end,
        received_bye=received_bye,
    )


def snapshot_server_traces(directory: Path, pattern: str) -> dict[Path, tuple[int, int]]:
    """Snapshot trace paths before a game so stale traces are never reused."""
    snapshot: dict[Path, tuple[int, int]] = {}
    if not directory.is_dir():
        return snapshot
    for path in directory.glob(pattern):
        if not path.is_file():
            continue
        try:
            stat = path.stat()
            snapshot[path.resolve()] = (stat.st_mtime_ns, stat.st_size)
        except OSError:
            continue
    return snapshot


def find_new_server_trace(
    directory: Path,
    pattern: str,
    before: dict[Path, tuple[int, int]],
    started_at_ns: int,
) -> Path | None:
    """Find the newest trace created or modified by this game only."""
    if not directory.is_dir():
        return None

    candidates: list[tuple[int, int, Path]] = []
    # A small tolerance avoids excluding a newly created file because of
    # filesystem timestamp precision or clock granularity on Windows.
    minimum_mtime_ns = started_at_ns - 2_000_000_000
    for path in directory.glob(pattern):
        if not path.is_file():
            continue
        try:
            stat = path.stat()
            resolved = path.resolve()
            current = (stat.st_mtime_ns, stat.st_size)
            changed = resolved not in before or before[resolved] != current
            if changed and stat.st_mtime_ns >= minimum_mtime_ns:
                candidates.append((stat.st_mtime_ns, stat.st_size, resolved))
        except OSError:
            continue

    if not candidates:
        return None
    return max(candidates, key=lambda item: (item[0], item[1]))[2]


def all_clients_protocol_complete(client_logs: list[Path]) -> bool:
    if len(client_logs) != 2:
        return False
    return all(
        (received_end and received_bye)
        for received_end, received_bye in
        (client_protocol_state(path) for path in client_logs)
    )


def terminate_process(process: subprocess.Popen[Any] | None) -> None:
    if process is None or process.poll() is not None:
        return
    process.terminate()
    try:
        process.wait(timeout=3)
    except subprocess.TimeoutExpired:
        process.kill()
        process.wait(timeout=3)


def run_one_game(
    game_number: int,
    config: dict[str, Any],
    settings: dict[str, str],
    output_root: Path,
) -> dict[str, Any]:
    game_dir = output_root / f"game_{game_number:03d}"
    game_dir.mkdir(parents=True, exist_ok=False)

    # Alternate connection order to reduce player-slot bias.
    if bool(config.get("alternate_connection_order", True)) and game_number % 2 == 0:
        slot_commands = [("B", config["client_b"]), ("A", config["client_a"])]
    else:
        slot_commands = [("A", config["client_a"]), ("B", config["client_b"])]

    server_exe = Path(config["server_exe"]).expanduser().resolve()
    server_log_path = game_dir / "server.log"
    server_trace_copy = game_dir / "server_trace.txt"
    client_logs = [game_dir / "player_0.log", game_dir / "player_1.log"]
    trace_dir = Path(config.get("server_trace_dir", str(server_exe.parent))).expanduser().resolve()
    trace_pattern = str(config.get("server_trace_glob", "trace_*.txt"))
    trace_snapshot = snapshot_server_traces(trace_dir, trace_pattern)

    server_process: subprocess.Popen[Any] | None = None
    client_processes: list[subprocess.Popen[Any] | None] = [None, None]
    client_return_codes: list[int | None] = [None, None]
    started_at = datetime.now(timezone.utc).isoformat()
    started_at_ns = time.time_ns()
    failure: str | None = None
    timed_out = False
    protocol_completed = False
    server_terminated_after_protocol = False
    server_shutdown_grace = max(0.0, float(config.get("server_exit_grace_seconds", 2.0)))
    client_grace = max(0.0, float(config.get("client_exit_grace_seconds", 10.0)))
    poll_interval = max(0.01, float(config.get("protocol_poll_interval_seconds", 0.10)))

    with server_log_path.open("w", encoding="utf-8", errors="replace") as server_log:
        client_streams = [
            path.open("w", encoding="utf-8", errors="replace") for path in client_logs
        ]
        try:
            # The optional override exists for the Python integration test,
            # which launches a short-lived fake server script on Windows too.
            # Normal benchmark configs omit it and launch the course EXE.
            server_command = config.get("server_command", [str(server_exe)])
            if not isinstance(server_command, list) or not server_command:
                raise ValueError("server_command must be a non-empty command array")
            server_process = subprocess.Popen(
                [str(value) for value in server_command],
                cwd=str(server_exe.parent),
                stdout=server_log,
                stderr=subprocess.STDOUT,
            )

            startup_delay = max(0.0, float(config.get("startup_delay_seconds", 2.0)))
            time.sleep(startup_delay)
            if server_process.poll() is not None:
                raise RuntimeError(
                    f"Server exited during startup with code {server_process.returncode}"
                )

            inter_client_delay = max(0.0, float(config.get("inter_client_delay_seconds", 0.25)))
            for slot, (_label, command) in enumerate(slot_commands):
                client_processes[slot] = subprocess.Popen(
                    [str(value) for value in command],
                    cwd=str(Path(command[0]).resolve().parent),
                    stdout=client_streams[slot],
                    stderr=subprocess.STDOUT,
                )
                if slot == 0 and inter_client_delay > 0:
                    time.sleep(inter_client_delay)

            duration_parts = [
                int(part) for part in settings.get("MaxGameDuration", "00:05:00").split(":")
            ]
            if len(duration_parts) == 3:
                configured_game_seconds = (
                    duration_parts[0] * 3600 + duration_parts[1] * 60 + duration_parts[2]
                )
            elif len(duration_parts) == 2:
                configured_game_seconds = duration_parts[0] * 60 + duration_parts[1]
            else:
                raise ValueError("MaxGameDuration must use HH:MM:SS or MM:SS")

            timeout = max(
                0.1,
                float(config.get("server_timeout_seconds", configured_game_seconds + 30.0)),
            )
            match_deadline = time.monotonic() + timeout

            # END and BYE are sent independently to both clients. Do not wait
            # for the server process: this server can keep its console process
            # alive after a completed match.
            while True:
                if all_clients_protocol_complete(client_logs):
                    protocol_completed = True
                    break

                finished_without_closure = [
                    slot
                    for slot, process in enumerate(client_processes)
                    if process is not None
                    and process.poll() is not None
                    and not all(client_protocol_state(client_logs[slot]))
                ]
                if finished_without_closure:
                    failure = (
                        "Client process(es) exited before receiving both END and BYE: "
                        + ", ".join(str(slot) for slot in finished_without_closure)
                    )
                    break

                if time.monotonic() >= match_deadline:
                    timed_out = True
                    failure = (
                        f"Match did not deliver END and BYE to both clients within "
                        f"{timeout:.1f} seconds"
                    )
                    break

                time.sleep(poll_interval)

            # Give both clients a short opportunity to close normally after BYE.
            if protocol_completed:
                for slot, process in enumerate(client_processes):
                    if process is None:
                        continue
                    try:
                        client_return_codes[slot] = process.wait(timeout=client_grace)
                    except subprocess.TimeoutExpired:
                        failure = failure or (
                            f"Player {slot} received END/BYE but did not exit within "
                            f"{client_grace:.1f} seconds"
                        )
                        terminate_process(process)
                        client_return_codes[slot] = process.returncode
            else:
                for slot, process in enumerate(client_processes):
                    if process is not None and process.poll() is not None:
                        client_return_codes[slot] = process.returncode

            # Some versions of the course server keep running after sending
            # END/BYE. Allow a brief graceful shutdown, then stop it as cleanup.
            if server_process is not None and server_process.poll() is None:
                try:
                    server_process.wait(timeout=server_shutdown_grace)
                except subprocess.TimeoutExpired:
                    if protocol_completed:
                        server_terminated_after_protocol = True
                    terminate_process(server_process)

        except Exception as error:  # Capture per-game failure and preserve logs.
            failure = f"{type(error).__name__}: {error}"
        finally:
            for slot, process in enumerate(client_processes):
                terminate_process(process)
                if process is not None and client_return_codes[slot] is None:
                    client_return_codes[slot] = process.poll()
            terminate_process(server_process)
            for stream in client_streams:
                stream.close()

    # The server writes its diagnostic trace beside the executable, not to
    # stdout. Copy (never move) the trace created/modified during this game.
    trace_source = find_new_server_trace(
        trace_dir, trace_pattern, trace_snapshot, started_at_ns
    )
    server_metrics_source = "unavailable"
    metrics_path = server_log_path
    if trace_source is not None:
        try:
            shutil.copy2(trace_source, server_trace_copy)
            metrics_path = server_trace_copy
            server_metrics_source = "server_trace_file"
        except OSError as error:
            failure = failure or f"Could not copy server trace {trace_source}: {error}"
    elif server_log_path.stat().st_size > 0:
        # Preserve compatibility with server builds that write diagnostics to
        # stdout/stderr. An empty stdout log is not treated as valid evidence.
        server_metrics_source = "server_stdout_fallback"

    server_metrics = parse_server_log(metrics_path)
    clients = []
    for slot, ((label, _command), log_path) in enumerate(zip(slot_commands, client_logs)):
        metrics = parse_client_log(log_path, label, client_return_codes[slot])
        clients.append(asdict(metrics) | {
            "decision_count": metrics.decision_count,
            "mean_ms": metrics.mean_ms,
            "max_ms": metrics.max_ms,
            "local_budget_overruns": metrics.local_budget_overruns,
        })

    # Protocol completion is established per connection. Server trace END/BYE
    # lines are corroborating evidence, not a prerequisite for client closure.
    completed = all(
        client["received_end"] and client["received_bye"]
        for client in clients
    )
    for slot, client in enumerate(clients):
        if client["return_code"] not in (None, 0) and failure is None:
            failure = (
                f"Player {slot} exited with return code {client['return_code']}"
            )
    if completed != protocol_completed:
        # Prefer the durable, post-run log parse if a client flushed its final
        # messages just after the polling loop observed process exit.
        protocol_completed = completed

    return {
        "game": game_number,
        "started_at_utc": started_at,
        "slot_0_agent": slot_commands[0][0],
        "slot_1_agent": slot_commands[1][0],
        "completed_protocol": completed,
        "winner": None,
        "winner_status": "not_exposed_by_verified_protocol",
        "timed_out": timed_out and not completed,
        "failure": failure,
        "server_return_code": server_process.returncode if server_process else None,
        "server_terminated_after_protocol": server_terminated_after_protocol,
        "server_metrics_source": server_metrics_source,
        "server_trace_source": str(trace_source) if trace_source else None,
        "server": asdict(server_metrics),
        "clients": clients,
        "logs": {
            "server": str(server_log_path),
            "server_trace": str(server_trace_copy) if trace_source else None,
            "player_0": str(client_logs[0]),
            "player_1": str(client_logs[1]),
        },
    }


def _write_csv(path: Path, games: list[dict[str, Any]]) -> None:
    columns = [
        "game", "slot_0_agent", "slot_1_agent", "completed_protocol",
        "winner", "winner_status", "server_metrics_source", "server_trace_found",
        "server_terminated_after_protocol", "plies", "mov_packets", "zero_move_packets",
        "malformed_mov_packets", "max_moves_in_one_packet", "rejection_markers",
        "player_0_species", "player_0_connected_and_initialized",
        "player_0_received_end", "player_0_received_bye", "player_0_decisions", "player_0_mean_ms",
        "player_0_max_ms", "player_0_local_budget_overruns",
        "player_1_species", "player_1_connected_and_initialized",
        "player_1_received_end", "player_1_received_bye", "player_1_decisions", "player_1_mean_ms",
        "player_1_max_ms", "player_1_local_budget_overruns", "failure",
    ]
    with path.open("w", newline="", encoding="utf-8") as stream:
        writer = csv.DictWriter(stream, fieldnames=columns)
        writer.writeheader()
        for game in games:
            server = game["server"]
            p0, p1 = game["clients"]
            writer.writerow({
                "game": game["game"],
                "slot_0_agent": game["slot_0_agent"],
                "slot_1_agent": game["slot_1_agent"],
                "completed_protocol": game["completed_protocol"],
                "winner": game["winner"],
                "winner_status": game["winner_status"],
                "server_metrics_source": game["server_metrics_source"],
                "server_trace_found": game["server_trace_source"] is not None,
                "server_terminated_after_protocol": game["server_terminated_after_protocol"],
                "plies": server["plies"],
                "mov_packets": server["mov_packets"],
                "zero_move_packets": server["zero_move_packets"],
                "malformed_mov_packets": server["malformed_mov_packets"],
                "max_moves_in_one_packet": server["max_moves_in_one_packet"],
                "rejection_markers": server["cheat_or_rejection_markers"],
                "player_0_species": p0["species"],
                "player_0_connected_and_initialized": p0["connected_and_initialized"],
                "player_0_received_end": p0["received_end"],
                "player_0_received_bye": p0["received_bye"],
                "player_0_decisions": p0["decision_count"],
                "player_0_mean_ms": p0["mean_ms"],
                "player_0_max_ms": p0["max_ms"],
                "player_0_local_budget_overruns": p0["local_budget_overruns"],
                "player_1_species": p1["species"],
                "player_1_connected_and_initialized": p1["connected_and_initialized"],
                "player_1_received_end": p1["received_end"],
                "player_1_received_bye": p1["received_bye"],
                "player_1_decisions": p1["decision_count"],
                "player_1_mean_ms": p1["mean_ms"],
                "player_1_max_ms": p1["max_ms"],
                "player_1_local_budget_overruns": p1["local_budget_overruns"],
                "failure": game["failure"],
            })


def _aggregate(games: list[dict[str, Any]]) -> dict[str, Any]:
    completed = [game for game in games if game["completed_protocol"]]
    all_timings: dict[str, list[float]] = {"A": [], "B": []}
    for game in games:
        for client in game["clients"]:
            all_timings[client["label"]].extend(client["timings_ms"])

    summary: dict[str, Any] = {
        "games_requested": len(games),
        "games_completed_protocol": len(completed),
        "games_timed_out": sum(1 for game in games if game["timed_out"]),
        "games_with_failures": sum(1 for game in games if game["failure"]),
        "games_with_server_trace": sum(1 for game in games if game["server_trace_source"]),
        "games_missing_server_trace": sum(1 for game in games if not game["server_trace_source"]),
        "winner_rate": None,
        "winner_rate_status": "unavailable_until_winner_signal_is_observable",
        "total_zero_move_packets": sum(g["server"]["zero_move_packets"] for g in games),
        "total_malformed_mov_packets": sum(g["server"]["malformed_mov_packets"] for g in games),
        "total_rejection_markers": sum(g["server"]["cheat_or_rejection_markers"] for g in games),
        "agent_latency_ms": {},
    }
    for label, values in all_timings.items():
        summary["agent_latency_ms"][label] = {
            "decision_count": len(values),
            "mean": statistics.fmean(values) if values else None,
            "max": max(values) if values else None,
            "p95": sorted(values)[max(0, math.ceil(0.95 * len(values)) - 1)] if values else None,
            "over_1500ms": sum(value > 1500.0 for value in values),
        }
    return summary


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--config", required=True, help="Path to benchmark JSON config")
    args = parser.parse_args()

    config_path = Path(args.config).expanduser().resolve()
    config = json.loads(config_path.read_text(encoding="utf-8"))
    settings = validate_config(config)

    if os.name != "nt":
        print(
            "This runner launches the verified .NET Framework game-server EXE. "
            "Run it on Windows; parser tests can run on macOS/Linux.",
            file=sys.stderr,
        )
        return 2

    output_root = Path(config["output_dir"]).expanduser().resolve()
    output_root.mkdir(parents=True, exist_ok=True)
    run_stamp = datetime.now().strftime("%Y%m%d_%H%M%S")
    run_dir = output_root / f"benchmark_{run_stamp}"
    run_dir.mkdir(parents=True, exist_ok=False)

    manifest = {
        "config": config,
        "server_settings": settings,
        "created_at_utc": datetime.now(timezone.utc).isoformat(),
        "host_platform": sys.platform,
    }
    (run_dir / "manifest.json").write_text(
        json.dumps(manifest, indent=2), encoding="utf-8"
    )

    games: list[dict[str, Any]] = []
    for game_number in range(1, int(config["games"]) + 1):
        print(f"[{game_number}/{config['games']}] Running match...")
        game = run_one_game(game_number, config, settings, run_dir)
        games.append(game)
        print(
            f"  protocol_complete={game['completed_protocol']} "
            f"plies={game['server']['plies']} "
            f"server_metrics_source={game['server_metrics_source']} "
            f"failure={game['failure'] or 'none'}"
        )
        (run_dir / f"game_{game_number:03d}.json").write_text(
            json.dumps(game, indent=2), encoding="utf-8"
        )

    summary = _aggregate(games)
    summary["games"] = games
    (run_dir / "summary.json").write_text(json.dumps(summary, indent=2), encoding="utf-8")
    _write_csv(run_dir / "summary.csv", games)

    print(f"\nResults written to: {run_dir}")
    print(f"Protocol-complete games: {summary['games_completed_protocol']}/{summary['games_requested']}")
    print(
        f"Games with server trace: {summary['games_with_server_trace']}/"
        f"{summary['games_requested']}"
    )
    print("Win rate: unavailable, the verified protocol does not expose a winner.")
    return 0 if summary["games_with_failures"] == 0 else 1


if __name__ == "__main__":
    raise SystemExit(main())
