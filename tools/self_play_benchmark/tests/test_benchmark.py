import os
import sys
import tempfile
import time
import unittest
from pathlib import Path

from benchmark import (
    all_clients_protocol_complete,
    find_new_server_trace,
    parse_client_log,
    parse_server_log,
    run_one_game,
    snapshot_server_traces,
)


class BenchmarkParserTests(unittest.TestCase):
    def test_server_log_metrics(self):
        text = """\
11/20/2026 11:48:24\tCoup n°1
\tMOV: en attente de 15 octets
11/20/2026 11:48:25\tCoup n°2
\tMOV: en attente de 0 octets
\tMOV: en attente de 11 octets
11/20/2026 11:49:56\tEnvoi de END et BYE et fermeture de la socket aux Vampires
11/20/2026 11:49:56\tEnvoi de END et BYE et fermeture de la socket aux Werewolves
"""
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "server.log"
            path.write_text(text, encoding="utf-8")
            metrics = parse_server_log(path)

        self.assertEqual(metrics.plies, 2)
        self.assertEqual(metrics.mov_packets, 3)
        self.assertEqual(metrics.total_mov_bytes, 26)
        self.assertEqual(metrics.zero_move_packets, 1)
        self.assertEqual(metrics.malformed_mov_packets, 1)
        self.assertEqual(metrics.max_moves_in_one_packet, 3)
        self.assertEqual(metrics.end_bye_messages, 2)

    def test_client_log_latency_species_and_closure(self):
        text = """\
[INFO] TCP connection established to 127.0.0.1:5555
[INFO] Game initialized: 20 rows x 20 columns; species=vampires
[TIMING] Decision and MOV-send processing=742 ms (local)
[TIMING] Decision and MOV-send processing=1601 ms (local)
[ERROR] sample error
[RX]   END (game finished)
[RX]   BYE (server is closing the session)
"""
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "player.log"
            path.write_text(text, encoding="utf-8")
            metrics = parse_client_log(path, "A", 0)

        self.assertEqual(metrics.species, "vampires")
        self.assertEqual(metrics.timings_ms, [742.0, 1601.0])
        self.assertEqual(metrics.error_lines, 1)
        self.assertEqual(metrics.local_budget_overruns, 1)
        self.assertTrue(metrics.connected_and_initialized)
        self.assertTrue(metrics.received_end)
        self.assertTrue(metrics.received_bye)
        self.assertEqual(metrics.return_code, 0)

    def test_missing_bye_is_not_complete_protocol(self):
        with tempfile.TemporaryDirectory() as directory:
            first = Path(directory) / "player_0.log"
            second = Path(directory) / "player_1.log"
            first.write_text("[RX]   END (game finished)\n", encoding="utf-8")
            second.write_text(
                "[RX]   END (game finished)\n[RX]   BYE (closing)\n",
                encoding="utf-8",
            )
            self.assertFalse(all_clients_protocol_complete([first, second]))

    def test_trace_discovery_ignores_unchanged_stale_files(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            old = root / "trace_old.txt"
            old.write_text("old", encoding="utf-8")
            before = snapshot_server_traces(root, "trace_*.txt")
            started_at_ns = time.time_ns()
            time.sleep(0.01)
            new = root / "trace_new.txt"
            new.write_text("new", encoding="utf-8")

            selected = find_new_server_trace(
                root, "trace_*.txt", before, started_at_ns
            )

            self.assertEqual(selected, new.resolve())

    def test_run_one_game_uses_trace_and_client_end_bye_not_server_exit(self):
        if os.name == "nt":
            server_name = "fake_server.py"
        else:
            server_name = "fake_server"

        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            server = root / server_name
            server.write_text(
                "#!/usr/bin/env python3\n"
                "from pathlib import Path\n"
                "import time\n"
                "Path('trace_fake-match.txt').write_text("
                "'Coup n°2\\nMOV: en attente de 15 octets\\n'"
                "+ 'Envoi de END et BYE et fermeture de la socket aux Vampires\\n'"
                "+ 'Envoi de END et BYE et fermeture de la socket aux Werewolves\\n', "
                "encoding='utf-8')\n"
                "time.sleep(30)\n",
                encoding="utf-8",
            )
            server.chmod(0o755)

            client = root / "fake_client.py"
            client.write_text(
                "import sys\n"
                "species = sys.argv[1]\n"
                "print('[INFO] TCP connection established', flush=True)\n"
                "print(f'[INFO] Game initialized: 20 rows x 20 columns; species={species}', flush=True)\n"
                "print('[RX]   END (game finished)', flush=True)\n"
                "print('[RX]   BYE (server is closing the session)', flush=True)\n",
                encoding="utf-8",
            )

            config = {
                "server_exe": str(server),
                "server_command": [sys.executable, str(server)],
                "server_trace_dir": str(root),
                "server_trace_glob": "trace_*.txt",
                "client_a": [sys.executable, str(client), "vampires"],
                "client_b": [sys.executable, str(client), "werewolves"],
                "alternate_connection_order": False,
                "startup_delay_seconds": 0.05,
                "inter_client_delay_seconds": 0.0,
                "server_timeout_seconds": 2.0,
                "client_exit_grace_seconds": 1.0,
                "server_exit_grace_seconds": 0.05,
                "protocol_poll_interval_seconds": 0.01,
            }
            settings = {"MaxGameDuration": "00:00:02"}
            output = root / "output"
            output.mkdir()

            started = time.monotonic()
            result = run_one_game(1, config, settings, output)
            elapsed = time.monotonic() - started

            self.assertTrue(result["completed_protocol"])
            self.assertFalse(result["timed_out"])
            self.assertIsNone(result["failure"])
            self.assertEqual(result["server_metrics_source"], "server_trace_file")
            self.assertTrue(result["server_terminated_after_protocol"])
            self.assertEqual(result["server"]["plies"], 2)
            self.assertEqual(result["server"]["mov_packets"], 1)
            self.assertEqual(result["server"]["max_moves_in_one_packet"], 3)
            self.assertTrue(Path(result["logs"]["server_trace"]).is_file())
            self.assertLess(elapsed, 1.5, "Runner waited for the server timeout unexpectedly")


if __name__ == "__main__":
    unittest.main()
