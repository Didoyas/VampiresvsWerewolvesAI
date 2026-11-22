import unittest

from analyze_capture import TcpSegment, decode_client_messages, reassemble_tcp_segments


class CaptureAnalysisTests(unittest.TestCase):
    def test_decodes_nme_and_multiple_mov_records(self):
        # NME "agent" then one MOV message containing two 5-byte movements.
        payload = (
            b"NME" + bytes([5]) + b"agent"
            + b"MOV" + bytes([2])
            + bytes([1, 2, 3, 4, 5, 6, 7, 8, 9, 10])
        )
        name, moves, incomplete = decode_client_messages(payload)
        self.assertEqual(name, "agent")
        self.assertEqual(moves, [(9, [[1, 2, 3, 4, 5], [6, 7, 8, 9, 10]])])
        self.assertEqual(incomplete, 0)

    def test_reassembles_fragmented_tcp_payload(self):
        first = TcpSegment(1, 1.0, 100, b"NME\x04te")
        second = TcpSegment(2, 1.1, 106, b"stMOV\x01\x01\x02\x03\x04\x05")
        blocks, gaps = reassemble_tcp_segments([first, second])
        self.assertEqual(gaps, 0)
        self.assertEqual(bytes(blocks[0].payload), b"NME\x04testMOV\x01\x01\x02\x03\x04\x05")
        name, moves, incomplete = decode_client_messages(bytes(blocks[0].payload))
        self.assertEqual(name, "test")
        self.assertEqual(moves, [(8, [[1, 2, 3, 4, 5]])])
        self.assertEqual(incomplete, 0)
        mov_offset, _ = moves[0]
        point = max((p for p in blocks[0].segment_points if p[0] <= mov_offset), key=lambda p: p[0])
        self.assertEqual(point[1:], (2, 1.1))

    def test_retransmission_is_not_decoded_twice(self):
        payload = b"NME\x01xMOV\x01\x01\x02\x03\x04\x05"
        segments = [
            TcpSegment(1, 1.0, 10, payload[:8]),
            TcpSegment(2, 1.1, 10, payload[:8]),
            TcpSegment(3, 1.2, 18, payload[8:]),
        ]
        blocks, gaps = reassemble_tcp_segments(segments)
        self.assertEqual(gaps, 0)
        name, moves, incomplete = decode_client_messages(bytes(blocks[0].payload))
        self.assertEqual(name, "x")
        self.assertEqual(len(moves), 1)
        self.assertEqual(moves[0][1], [[1, 2, 3, 4, 5]])
        self.assertEqual(incomplete, 0)

    def test_reports_gap_instead_of_fabricating_bytes(self):
        segments = [
            TcpSegment(1, 1.0, 100, b"NME\x01x"),
            TcpSegment(2, 1.1, 110, b"MOV\x01\x01\x02\x03\x04\x05"),
        ]
        blocks, gaps = reassemble_tcp_segments(segments)
        self.assertEqual(gaps, 1)
        self.assertEqual(len(blocks), 2)


if __name__ == "__main__":
    unittest.main()
