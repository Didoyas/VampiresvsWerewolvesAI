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


def parse_client_log(path: Path, label: str, return_code: int | None) -> ClientMetrics:
    timings: list[float] = []
    species = "unknown"
    error_lines = 0
    timing_prefix = "[TIMING] Decision and MOV-send processing="

    if path.is_file():
        with path.open("r", encoding="utf-8", errors="replace") as stream:
            for line in stream:
                if "[ERROR]" in line:
                    error_lines += 1

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

    return ClientMetrics(
        label=label,
        species=species,
        timings_ms=timings,
        error_lines=error_lines,
        return_code=return_code,
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

    server_log_path = game_dir / "server.log"
    client_logs = [game_dir / "player_0.log", game_dir / "player_1.log"]
    server_process: subprocess.Popen[Any] | None = None
    client_processes: list[subprocess.Popen[Any] | None] = [None, None]
    client_return_codes: list[int | None] = [None, None]
    started_at = datetime.now(timezone.utc).isoformat()
    failure: str | None = None
    timed_out = False

    with server_log_path.open("w", encoding="utf-8", errors="replace") as server_log:
        client_streams = [
            path.open("w", encoding="utf-8", errors="replace") for path in client_logs
        ]
        try:
            server_process = subprocess.Popen(
                [str(Path(config["server_exe"]).resolve())],
                cwd=str(Path(config["server_exe"]).resolve().parent),
                stdout=server_log,
                stderr=subprocess.STDOUT,
            )

            startup_delay = float(config.get("startup_delay_seconds", 2.0))
            time.sleep(max(0.0, startup_delay))
            if server_process.poll() is not None:
                raise RuntimeError(
                    f"Server exited during startup with code {server_process.returncode}"
                )

            inter_client_delay = float(config.get("inter_client_delay_seconds", 0.25))
            for slot, (label, command) in enumerate(slot_commands):
                client_processes[slot] = subprocess.Popen(
                    [str(value) for value in command],
                    cwd=str(Path(command[0]).resolve().parent),
                    stdout=client_streams[slot],
                    stderr=subprocess.STDOUT,
                )
                if slot == 0 and inter_client_delay > 0:
                    time.sleep(inter_client_delay)

            duration_parts = [int(part) for part in settings.get("MaxGameDuration", "00:05:00").split(":")]
            if len(duration_parts) == 3:
                configured_game_seconds = (
                    duration_parts[0] * 3600 + duration_parts[1] * 60 + duration_parts[2]
                )
            elif len(duration_parts) == 2:
                configured_game_seconds = duration_parts[0] * 60 + duration_parts[1]
            else:
                raise ValueError("MaxGameDuration must use HH:MM:SS or MM:SS")
            timeout = float(
                config.get("server_timeout_seconds", configured_game_seconds + 30.0)
            )
            try:
                server_process.wait(timeout=timeout)
            except subprocess.TimeoutExpired:
                timed_out = True
                failure = f"Server exceeded timeout of {timeout:.1f} seconds"
                terminate_process(server_process)

            client_grace = float(config.get("client_exit_grace_seconds", 10.0))
            for slot, process in enumerate(client_processes):
                if process is None:
                    continue
                try:
                    client_return_codes[slot] = process.wait(timeout=client_grace)
                except subprocess.TimeoutExpired:
                    failure = failure or f"Player {slot} did not exit after server shutdown"
                    terminate_process(process)
                    client_return_codes[slot] = process.returncode

        except Exception as error:  # Capture per-game failure and preserve logs.
            failure = f"{type(error).__name__}: {error}"
        finally:
            for process in client_processes:
                terminate_process(process)
            terminate_process(server_process)
            for stream in client_streams:
                stream.close()

    server_metrics = parse_server_log(server_log_path)
    clients = []
    for slot, ((label, _command), log_path) in enumerate(zip(slot_commands, client_logs)):
        metrics = parse_client_log(log_path, label, client_return_codes[slot])
        clients.append(asdict(metrics) | {
            "decision_count": metrics.decision_count,
            "mean_ms": metrics.mean_ms,
            "max_ms": metrics.max_ms,
            "local_budget_overruns": metrics.local_budget_overruns,
        })

    # The server's END/BYE frames do not include the winner/final board.
    # A complete protocol termination is therefore not equivalent to a result.
    completed = server_metrics.end_bye_messages >= 2 and not timed_out
    return {
        "game": game_number,
        "started_at_utc": started_at,
        "slot_0_agent": slot_commands[0][0],
        "slot_1_agent": slot_commands[1][0],
        "completed_protocol": completed,
        "winner": None,
        "winner_status": "not_exposed_by_verified_protocol",
        "timed_out": timed_out,
        "failure": failure,
        "server_return_code": server_process.returncode if server_process else None,
        "server": asdict(server_metrics),
        "clients": clients,
        "logs": {
            "server": str(server_log_path),
            "player_0": str(client_logs[0]),
            "player_1": str(client_logs[1]),
        },
    }


def _write_csv(path: Path, games: list[dict[str, Any]]) -> None:
    columns = [
        "game", "slot_0_agent", "slot_1_agent", "completed_protocol",
        "winner", "winner_status", "plies", "mov_packets", "zero_move_packets",
        "malformed_mov_packets", "max_moves_in_one_packet", "rejection_markers",
        "player_0_species", "player_0_decisions", "player_0_mean_ms",
        "player_0_max_ms", "player_0_local_budget_overruns",
        "player_1_species", "player_1_decisions", "player_1_mean_ms",
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
                "plies": server["plies"],
                "mov_packets": server["mov_packets"],
                "zero_move_packets": server["zero_move_packets"],
                "malformed_mov_packets": server["malformed_mov_packets"],
                "max_moves_in_one_packet": server["max_moves_in_one_packet"],
                "rejection_markers": server["cheat_or_rejection_markers"],
                "player_0_species": p0["species"],
                "player_0_decisions": p0["decision_count"],
                "player_0_mean_ms": p0["mean_ms"],
                "player_0_max_ms": p0["max_ms"],
                "player_0_local_budget_overruns": p0["local_budget_overruns"],
                "player_1_species": p1["species"],
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
    print("Win rate: unavailable, the verified protocol does not expose a winner.")
    return 0 if summary["games_with_failures"] == 0 else 1


if __name__ == "__main__":
    raise SystemExit(main())
