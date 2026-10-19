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


class CaptureError(RuntimeError):
	pass


def start(seconds: float, channels: str = "D0,D1,D2,D3", samplerate: str = "1M", driver: str = "fx2lafw") -> tuple[subprocess.Popen, Path]:
	"""Spusti zachyt na pozadi; vraci (proces, .sr soubor). Dokonci ho pomoci finish()."""
	out = Path(tempfile.mktemp(suffix=".sr", dir=_DIR))
	proc = subprocess.Popen(
		["sigrok-cli", "-d", driver, "--channels", channels, "--config", f"samplerate={samplerate}", "--time", f"{int(seconds * 1000)}ms", "-o", str(out)],
		stdout=subprocess.DEVNULL,
		stderr=subprocess.PIPE,
		text=True,
	)
	return proc, out


def finish(proc: subprocess.Popen, out: Path, timeout: float) -> Path:
	"""Pocka na zachyt; pri chybe/timeoutu proces zabije a vyhodi CaptureError se stderr."""
	try:
		_, err = proc.communicate(timeout=timeout)
	except subprocess.TimeoutExpired:
		proc.kill()
		_, err = proc.communicate()
		raise CaptureError(f"sigrok-cli timeout po {timeout:.0f} s: {err.strip()}") from None
	finally:
		if proc.poll() is None:
			proc.kill()
			proc.communicate()
	if proc.returncode != 0:
		raise CaptureError(f"sigrok-cli skoncil s kodem {proc.returncode}: {err.strip()}")
	if not out.exists() or out.stat().st_size == 0:
		raise CaptureError(f"zachyt je prazdny nebo chybi ({out}): {err.strip()}")
	return out


def capture(seconds: float, channels: str = "D0,D1,D2,D3", samplerate: str = "1M", driver: str = "fx2lafw") -> Path:
	"""Blokujici zachyt; vraci cestu k .sr souboru, pri selhani vyhodi CaptureError."""
	proc, out = start(seconds, channels, samplerate, driver)
	return finish(proc, out, seconds + 30)


def uart(sr: Path, rx: str, baud: int) -> bytes:
	"""Dekoduje bajty UART na kanalu `rx` (napr. CH["base_ble_tx"]); pri chybe sigrok-cli vyhodi CaptureError."""
	r = subprocess.run(["sigrok-cli", "-i", str(sr), "-P", f"uart:rx={rx}:baudrate={baud}", "-A", "uart=rx-data"], capture_output=True, text=True)
	if r.returncode != 0:
		raise CaptureError(f"dekodovani {sr} selhalo (kod {r.returncode}): {r.stderr.strip()}")
	data = bytearray()
	for line in r.stdout.splitlines():
		tok = line.split()[-1] if line.strip() else ""
		if len(tok) == 2:
			data.append(int(tok, 16))
	return bytes(data)


if __name__ == "__main__":
	import sys

	sr = capture(float(sys.argv[1]) if len(sys.argv) > 1 else 3)
	for name, baud in (("disp_tx", 1200), ("disp_rx", 1200), ("base_ble_tx", 115200), ("base_ble_rx", 115200)):
		print(name, uart(sr, CH[name], baud))
