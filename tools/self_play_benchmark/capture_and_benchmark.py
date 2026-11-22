#!/usr/bin/env python3
"""Capture game TCP traffic while running the existing Windows benchmark.

Starts dumpcap, runs benchmark.py unchanged, stops dumpcap cleanly, then runs
analyze_capture.py. Requires Windows, Python 3.10+, Wireshark/Npcap.
"""

from __future__ import annotations

import argparse
import json
import os
import shutil
import signal
import subprocess
import sys
import time
from datetime import datetime
from pathlib import Path


def find_executable(explicit: str | None, name: str, extra: list[str]) -> str:
    candidates = [explicit, shutil.which(name), *extra]
    for candidate in candidates:
        if candidate and Path(candidate).is_file():
            return str(Path(candidate).resolve())
    raise FileNotFoundError(
        f"{name} was not found. Install Wireshark or pass --{name} PATH."
    )


def load_port(config_path: Path) -> int:
    config = json.loads(config_path.read_text(encoding="utf-8"))
    for key in ("client_a", "client_b"):
        command = config.get(key)
        if not isinstance(command, list) or len(command) < 3:
            raise ValueError(f"{key} must be a command array ending with IP and port")
    port_a = int(config["client_a"][-1])
    port_b = int(config["client_b"][-1])
    if port_a != port_b:
        raise ValueError("client_a and client_b must use the same server port")
    return port_a


def stop_dumpcap(process: subprocess.Popen, timeout: float = 10.0) -> None:
    if process.poll() is not None:
        return
    if os.name == "nt":
        try:
            process.send_signal(signal.CTRL_BREAK_EVENT)
            process.wait(timeout=timeout)
            return
        except (OSError, subprocess.TimeoutExpired):
            pass
    else:
        try:
            process.send_signal(signal.SIGINT)
            process.wait(timeout=timeout)
            return
        except (OSError, subprocess.TimeoutExpired):
            pass
    process.terminate()
    try:
        process.wait(timeout=3)
    except subprocess.TimeoutExpired:
        process.kill()
        process.wait(timeout=3)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--config", required=True, help="Existing benchmark_config.json")
    parser.add_argument(
        "--interface", action="append", required=True,
        help="dumpcap interface number/name from dumpcap -D; repeat to capture multiple interfaces",
    )
    parser.add_argument("--dumpcap", help="Optional path to dumpcap.exe")
    parser.add_argument("--tshark", help="Optional path to tshark.exe")
    parser.add_argument("--skip-analysis", action="store_true", help="Keep pcap but do not decode it")
    args = parser.parse_args()

    if os.name != "nt":
        print("This orchestration script launches the Windows .NET server benchmark; run it on Windows.", file=sys.stderr)
        return 2

    config_path = Path(args.config).expanduser().resolve()
    benchmark_script = Path(__file__).with_name("benchmark.py")
    analyzer_script = Path(__file__).with_name("analyze_capture.py")
    if not config_path.is_file():
        parser.error(f"Benchmark config not found: {config_path}")
    if not benchmark_script.is_file() or not analyzer_script.is_file():
        parser.error("benchmark.py or analyze_capture.py is missing from this folder")

    try:
        port = load_port(config_path)
        dumpcap = find_executable(
            args.dumpcap, "dumpcap",
            [r"C:\Program Files\Wireshark\dumpcap.exe", r"C:\Program Files (x86)\Wireshark\dumpcap.exe"],
        )
        tshark = find_executable(
            args.tshark, "tshark",
            [r"C:\Program Files\Wireshark\tshark.exe", r"C:\Program Files (x86)\Wireshark\tshark.exe"],
        )
    except (FileNotFoundError, ValueError, json.JSONDecodeError) as error:
        print(f"ERROR: {error}", file=sys.stderr)
        return 2

    # Keep output_dir semantics identical to benchmark.py: relative paths are
    # resolved from the shell's current working directory.
    config = json.loads(config_path.read_text(encoding="utf-8"))
    output_root = Path(config.get("output_dir", "benchmark_results")).expanduser()
    if not output_root.is_absolute():
        output_root = (Path.cwd() / output_root).resolve()
    capture_dir = output_root / "captures"
    capture_dir.mkdir(parents=True, exist_ok=True)
    stamp = datetime.now().strftime("%Y%m%d_%H%M%S")
    capture_path = capture_dir / f"vvw_capture_{stamp}.pcapng"
    dumpcap_log = capture_dir / f"vvw_capture_{stamp}_dumpcap.log"
    analysis_dir = capture_dir / f"vvw_capture_{stamp}_analysis"

    dumpcap_command = [dumpcap]
    for interface in args.interface:
        dumpcap_command.extend(["-i", interface])
    dumpcap_command.extend(["-f", f"tcp port {port}", "-w", str(capture_path)])
    print("Starting packet capture:")
    print(f"  Interfaces: {', '.join(args.interface)}")
    print(f"  TCP port:   {port}")
    print(f"  File:       {capture_path}")

    process: subprocess.Popen | None = None
    benchmark_return_code = 2
    with dumpcap_log.open("w", encoding="utf-8", errors="replace") as log_handle:
        try:
            creation_flags = subprocess.CREATE_NEW_PROCESS_GROUP if os.name == "nt" else 0
            process = subprocess.Popen(
                dumpcap_command,
                stdin=subprocess.DEVNULL,
                stdout=log_handle,
                stderr=subprocess.STDOUT,
                creationflags=creation_flags,
            )
            time.sleep(2.0)
            if process.poll() is not None:
                log_handle.flush()
                print("ERROR: dumpcap stopped during startup. Capture log:", dumpcap_log, file=sys.stderr)
                print(dumpcap_log.read_text(encoding="utf-8", errors="replace"), file=sys.stderr)
                return 2

            print("Capture is active. Starting the existing benchmark.py now.\n")
            benchmark_command = [
                sys.executable, str(benchmark_script), "--config", str(config_path)
            ]
            completed = subprocess.run(benchmark_command, cwd=Path.cwd())
            benchmark_return_code = completed.returncode
        except KeyboardInterrupt:
            print("\nInterrupted. Stopping capture and preserving the pcap file.", file=sys.stderr)
            benchmark_return_code = 130
        finally:
            if process is not None:
                stop_dumpcap(process)

    if not capture_path.is_file() or capture_path.stat().st_size == 0:
        print("ERROR: no capture file was written. See", dumpcap_log, file=sys.stderr)
        return 2

    print(f"\nCapture saved: {capture_path}")
    if not args.skip_analysis:
        analysis_command = [
            sys.executable, str(analyzer_script),
            "--pcap", str(capture_path),
            "--port", str(port),
            "--outdir", str(analysis_dir),
            "--tshark", tshark,
        ]
        analysis_result = subprocess.run(analysis_command, cwd=Path.cwd())
        if analysis_result.returncode != 0:
            print("Capture retained. Analysis did not complete successfully.", file=sys.stderr)
            return analysis_result.returncode

    print(f"dumpcap log: {dumpcap_log}")
    print("Interpret MOV records as submitted commands only, not confirmed server moves.")
    return benchmark_return_code


if __name__ == "__main__":
    raise SystemExit(main())
