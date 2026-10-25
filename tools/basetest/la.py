"""Logic analyzer (fx2lafw via sigrok-cli): capture of the base buses and UART decoding."""

import subprocess
import tempfile
from pathlib import Path

# sigrok D0..D7 = CH1..CH8; directions verified on the bench 2026-10-19:
# display D1 = MCU -> display, D0 = display -> MCU; BLE D3 = MCU -> RN4870, D2 = RN4870 -> MCU;
# remote BLE (Task 5, %CONNECT%/%SECURED% of the module on D6, "$$$"/"A" of the MCU on D7): D7 = MCU -> RN4870, D6 = RN4870 -> MCU
CH = {
	"disp_tx": "D1",
	"disp_rx": "D0",
	"base_ble_tx": "D3",
	"base_ble_rx": "D2",
	"rem_scl": "D4",
	"rem_sda": "D5",
	"rem_ble_tx": "D7",
	"rem_ble_rx": "D6",
}

_DIR = Path(tempfile.mkdtemp(prefix="basetest-la-"))


class CaptureError(RuntimeError):
	pass


def start(seconds: float, channels: str = "D0,D1,D2,D3", samplerate: str = "1M", driver: str = "fx2lafw") -> tuple[subprocess.Popen, Path]:
	"""Starts a capture in the background; returns (process, .sr file). Complete it with finish()."""
	out = Path(tempfile.mktemp(suffix=".sr", dir=_DIR))
	proc = subprocess.Popen(
		["sigrok-cli", "-d", driver, "--channels", channels, "--config", f"samplerate={samplerate}", "--time", f"{int(seconds * 1000)}ms", "-o", str(out)],
		stdout=subprocess.DEVNULL,
		stderr=subprocess.PIPE,
		text=True,
	)
	return proc, out


def finish(proc: subprocess.Popen, out: Path, timeout: float) -> Path:
	"""Waits for the capture; on an error/timeout kills the process and raises CaptureError with stderr."""
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
	"""Blocking capture; returns the path to the .sr file, raises CaptureError on failure."""
	proc, out = start(seconds, channels, samplerate, driver)
	return finish(proc, out, seconds + 30)


def uart(sr: Path, rx: str, baud: int) -> bytes:
	"""Decodes UART bytes on channel `rx` (e.g. CH["base_ble_tx"]); raises CaptureError if sigrok-cli fails."""
	r = subprocess.run(["sigrok-cli", "-i", str(sr), "-P", f"uart:rx={rx}:baudrate={baud}", "-A", "uart=rx-data"], capture_output=True, text=True)
	if r.returncode != 0:
		raise CaptureError(f"dekodovani {sr} selhalo (kod {r.returncode}): {r.stderr.strip()}")
	data = bytearray()
	for line in r.stdout.splitlines():
		tok = line.split()[-1] if line.strip() else ""
		if len(tok) == 2:
			data.append(int(tok, 16))
	return bytes(data)


def uart_timed(sr: Path, rx: str, baud: int, samplerate: float) -> tuple[list[tuple[float, int]], list[tuple[float, str]]]:
	"""Like uart(), but with time: returns ([(byte_start_time_s, byte)], [(time_s, warning text)]).
	Warnings (rx-warnings: framing error, parity error, ...) are UART decoding errors; an empty list = a clean recording.
	`samplerate` must match the capture (a number in Hz, e.g. 100000)."""
	r = subprocess.run(
		["sigrok-cli", "-i", str(sr), "-P", f"uart:rx={rx}:baudrate={baud}", "-A", "uart=rx-data:rx-warnings", "--protocol-decoder-samplenum"],
		capture_output=True, text=True)
	if r.returncode != 0:
		raise CaptureError(f"dekodovani {sr} selhalo (kod {r.returncode}): {r.stderr.strip()}")
	data, warnings = [], []
	for line in r.stdout.splitlines():
		head, sep, text = line.partition(" uart-1: ")
		if not sep:
			continue
		t = int(head.split("-")[0]) / samplerate
		text = text.strip()
		if len(text) == 2 and all(c in "0123456789ABCDEFabcdef" for c in text):
			data.append((t, int(text, 16)))
		else:
			warnings.append((t, text))
	return data, warnings


if __name__ == "__main__":
	import sys

	sr = capture(float(sys.argv[1]) if len(sys.argv) > 1 else 3)
	for name, baud in (("disp_tx", 1200), ("disp_rx", 1200), ("base_ble_tx", 115200), ("base_ble_rx", 115200)):
		print(name, uart(sr, CH[name], baud))
