import tempfile
import unittest
from pathlib import Path

from benchmark import parse_client_log, parse_server_log


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

    def test_client_log_latency_and_species(self):
        text = """\
[INFO] Game initialized: 20 rows x 20 columns; species=vampires
[TIMING] Decision and MOV-send processing=742 ms (local)
[TIMING] Decision and MOV-send processing=1601 ms (local)
[ERROR] sample error
"""
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "player.log"
            path.write_text(text, encoding="utf-8")
            metrics = parse_client_log(path, "A", 0)

        self.assertEqual(metrics.species, "vampires")
        self.assertEqual(metrics.timings_ms, [742.0, 1601.0])
        self.assertEqual(metrics.error_lines, 1)
        self.assertEqual(metrics.local_budget_overruns, 1)
        self.assertEqual(metrics.return_code, 0)


if __name__ == "__main__":
    unittest.main()
