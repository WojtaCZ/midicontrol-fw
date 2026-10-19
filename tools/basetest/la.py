"""Logic analyzer (fx2lafw via sigrok-cli): zachyt sbernic base a dekodovani UART."""

import subprocess
import tempfile
from pathlib import Path

# sigrok D0..D7 = CH1..CH8; smery overeny na lavici 2026-10-19:
# displej D1 = MCU -> displej, D0 = displej -> MCU; BLE D3 = MCU -> RN4870, D2 = RN4870 -> MCU
CH = {
	"disp_tx": "D1",
	"disp_rx": "D0",
	"base_ble_tx": "D3",
	"base_ble_rx": "D2",
	"rem_scl": "D4",
	"rem_sda": "D5",
	"rem_ble_tx": "D6",
	"rem_ble_rx": "D7",
}

_DIR = Path(tempfile.mkdtemp(prefix="basetest-la-"))


def start(seconds: float, channels: str = "D0,D1,D2,D3", samplerate: str = "1M") -> tuple[subprocess.Popen, Path]:
	"""Spusti zachyt na pozadi; vraci (proces, .sr soubor). Pockej na proces a pak dekoduj."""
	out = Path(tempfile.mktemp(suffix=".sr", dir=_DIR))
	proc = subprocess.Popen(
		["sigrok-cli", "-d", "fx2lafw", "--channels", channels, "--config", f"samplerate={samplerate}", "--time", f"{int(seconds * 1000)}ms", "-o", str(out)],
		stdout=subprocess.DEVNULL,
		stderr=subprocess.DEVNULL,
	)
	return proc, out


def capture(seconds: float, channels: str = "D0,D1,D2,D3", samplerate: str = "1M") -> Path:
	"""Blokujici zachyt; vraci cestu k .sr souboru."""
	proc, out = start(seconds, channels, samplerate)
	proc.wait(seconds + 30)
	return out


def uart(sr: Path, rx: str, baud: int) -> bytes:
	"""Dekoduje bajty UART na kanalu `rx` (napr. CH["base_ble_tx"])."""
	out = subprocess.run(
		["sigrok-cli", "-i", str(sr), "-P", f"uart:rx={rx}:baudrate={baud}", "-A", "uart=rx-data"], capture_output=True, text=True, check=True
	).stdout
	data = bytearray()
	for line in out.splitlines():
		tok = line.split()[-1] if line.strip() else ""
		if len(tok) == 2:
			data.append(int(tok, 16))
	return bytes(data)


if __name__ == "__main__":
	import sys

	sr = capture(float(sys.argv[1]) if len(sys.argv) > 1 else 3)
	for name, baud in (("disp_tx", 1200), ("disp_rx", 1200), ("base_ble_tx", 115200), ("base_ble_rx", 115200)):
		print(name, uart(sr, CH[name], baud))
