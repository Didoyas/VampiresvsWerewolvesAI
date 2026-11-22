# Windows teammate handoff: benchmark with TCP movement capture

Follow the complete instructions in [README.md](README.md), especially:

- **A. Prepare the existing benchmark** to configure the server and two clients.
- **B. Identify the correct capture interface** using `dumpcap -D`.
- **C. Run benchmark and capture together** using `capture_and_benchmark.py`.
- **F. Troubleshooting** if the capture contains no MOV messages.

Quick start from this directory in PowerShell:

```powershell
py -3 -m unittest discover -s .\tests -v
& "C:\Program Files\Wireshark\dumpcap.exe" -D
py -3 .\capture_and_benchmark.py --config .\benchmark_config.json --interface <INTERFACE_NUMBER>
```

Replace `<INTERFACE_NUMBER>` with the number for the correct adapter. For
`127.0.0.1` connections, try **Adapter for loopback traffic capture**.

Important: the capture analyzer decodes client-submitted MOV commands. It does
not prove that the server accepted or executed them. To observe both players on
the competition network, capture on the server host or an authorized mirrored
network point, not only on one player's laptop.
