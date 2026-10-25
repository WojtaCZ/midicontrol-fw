#include "debug.hpp"
#include "resetcause.hpp"
#include "git.hpp"
#include "fault.hpp"
#include "crc32.hpp"

#include <cstdarg>
#include <cstdio>
#include <cstring>

#include <stm32g431xx.h>
#include <core_cm4.h>
#include <cmsis_compiler.h>

#include <tusb.h>
#include <tinyusb/src/class/cdc/cdc_device.h>

#include "stmcpp/systick.hpp"

namespace Debug {

	// ── Ring in no-init RAM (0x20007800, NOINIT in the linker script of both the application and the bootloader) ──
	// head/tail are absolute positions (they keep growing), index = position % RING_SIZE. The ring holds the last
	// RING_SIZE bytes of history regardless of what has already gone out to the CDC: after a reset all of it is printed as
	// "log before reset". Whatever is overwritten before it was sent is counted in `dropped` (lines).
	static constexpr uint32_t RING_MAGIC = 0x4C4F4721; // "!GOL"
	static constexpr uint32_t RING_SIZE = 1536;
	static constexpr uint8_t LOG_CDC = 1; // TinyUSB instance log CDC
	static constexpr size_t MAX_TEXT = 120;

	struct Header {
		uint32_t magic;
		uint32_t head;
		uint32_t tail;
		uint32_t dropped;
		uint32_t crc; // CRC-32 of the previous 4 fields
	};

	struct NoInitLog {
		Header h;
		char ring[RING_SIZE];
	};

	// used: tests (dbg.debug_log) read it through the probe via the symbol table
	[[gnu::section(".noinit.log"), gnu::used]] NoInitLog _log;

	static Level _level = Level::INFO;
	static uint32_t _sent = 0;    // absolute position: what has already gone out to the CDC (tail <= _sent <= head)
	static uint32_t _bootPos = 0; // head at the moment of init(): boundary of the log before the reset

	// State of printing the log before the reset
	enum class PreReset : uint8_t { NONE, START, BODY, DONE };
	static PreReset _pre = PreReset::NONE;

	// Port open (DTR) and since when: the first lines are sent only after HOLD_OFF_MS, because the PC clears its
	// input buffer after opening the port (pyserial and termios tcflush) and anything arriving right after DTR would be lost
	static constexpr uint32_t HOLD_OFF_MS = 200;
	static bool _dtr = false;
	static uint32_t _dtrSince = 0;

	static volatile uint16_t _isrCounts[ISR_EVENT_COUNT] = {};
	static uint32_t _lastIsrCheck = 0;

	// Response to a command (sent with priority, outside the ring)
	static char _reply[96];
	static uint8_t _replyLen = 0;
	static char _cmd[32];
	// Printing the fault record with the "dump" command: the next line (fault::formatLine) only after the previous one has been sent
	static constexpr uint8_t DUMP_NONE = 0xFF;
	static uint8_t _dumpIdx = DUMP_NONE;
	static uint8_t _cmdLen = 0;

	// ── Header CRC ────────────────────────────────────────────────────────

	static uint32_t headerCrc(const Header &h) { return util::crc32(&h, offsetof(Header, crc)); }

	static void seal() { _log.h.crc = headerCrc(_log.h); }

	static bool headerValid(const Header &h) {
		return h.magic == RING_MAGIC && h.crc == headerCrc(h) && (h.head - h.tail) <= RING_SIZE;
	}

	// Comparison of absolute positions robust against uint32 overflow (after 4 GB of log)
	static bool before(uint32_t a, uint32_t b) { return static_cast<int32_t>(a - b) < 0; }

	static char at(uint32_t pos) { return _log.ring[pos % RING_SIZE]; }

	// ── Writing to the ring ───────────────────────────────────────────────

	// Frees room for `need` bytes in whole lines (oldest first)
	static void makeRoom(uint32_t need) {
		Header &h = _log.h;
		uint32_t tail = h.tail;
		uint32_t dropped = 0;
		while (h.head + need - tail > RING_SIZE) {
			uint32_t lineStart = tail;
			while (tail != h.head && at(tail) != '\n') tail++;
			if (tail == h.head) break; // no end of line (should not happen): drop everything
			tail++;
			if (!before(lineStart, _sent)) dropped++; // line was not sent
		}
		if (before(_sent, tail)) _sent = tail;
		// Header (tail) first, only then are bytes overwritten: a reset in between leaves a valid state
		h.tail = tail;
		h.dropped += dropped;
		seal();
	}

	static void append(const char *data, uint32_t len) {
		if (len == 0 || len > RING_SIZE) return;
		Header &h = _log.h;
		if (h.tail >= 0x80000000u) {
			// 2^32 is not a multiple of RING_SIZE: shift the positions in time by a multiple of RING_SIZE (indices and differences are preserved)
			uint32_t k = (h.tail / RING_SIZE) * RING_SIZE;
			h.head -= k;
			h.tail -= k;
			_sent -= k;
			_bootPos -= k;
			seal();
		}
		makeRoom(len);
		for (uint32_t i = 0; i < len; ++i) _log.ring[(h.head + i) % RING_SIZE] = data[i];
		h.head += len;
		seal();
	}

	static char levelLetter(Level lvl) {
		switch (lvl) {
			case Level::ERROR: return 'E';
			case Level::WARNING: return 'W';
			case Level::INFO: return 'I';
			default: return 'D';
		}
	}

	__attribute__((format(printf, 3, 0))) static void vlog(Level lvl, const char *tag, const char *fmt, va_list ap) {
		char line[24 + MAX_TEXT + 4];
		uint32_t ms = stmcpp::systick::getTicks();
		int n = snprintf(line, sizeof(line), "[%6lu.%03lu] %c %.6s: ", static_cast<unsigned long>(ms / 1000), static_cast<unsigned long>(ms % 1000),
						 levelLetter(lvl), tag);
		if (n <= 0) return;
		size_t pre = static_cast<size_t>(n);
		int t = vsnprintf(line + pre, MAX_TEXT + 1, fmt, ap);
		if (t < 0) t = 0;
		size_t text = static_cast<size_t>(t) > MAX_TEXT ? MAX_TEXT : static_cast<size_t>(t);
		// A line must remain a single line
		for (size_t i = pre; i < pre + text; ++i) {
			if (line[i] == '\r' || line[i] == '\n') line[i] = ' ';
		}
		size_t len = pre + text;
		if (static_cast<size_t>(t) > MAX_TEXT) line[len++] = '~';
		line[len++] = '\r';
		line[len++] = '\n';
		append(line, static_cast<uint32_t>(len));
	}

	static void logForce(Level lvl, const char *tag, const char *fmt, ...) __attribute__((format(printf, 3, 4)));
	static void logForce(Level lvl, const char *tag, const char *fmt, ...) {
		va_list ap;
		va_start(ap, fmt);
		vlog(lvl, tag, fmt, ap);
		va_end(ap);
	}

	// ── Public API ────────────────────────────────────────────────────────

	void log(Level lvl, const char *tag, const char *fmt, ...) {
		// The ring must not be written from an interrupt (the main loop may be in the middle of append): only count it
		if (__get_IPSR() != 0) {
			logFromIsr(ISR_LOG_FROM_ISR);
			return;
		}
		if (lvl == Level::NONE || lvl > _level) return;
		va_list ap;
		va_start(ap, fmt);
		vlog(lvl, tag, fmt, ap);
		va_end(ap);
	}

	void logFault(const char *fmt, ...) {
		va_list ap;
		va_start(ap, fmt);
		vlog(Level::ERROR, "fault", fmt, ap);
		va_end(ap);
	}

	void logFromIsr(uint8_t eventCode) {
		if (eventCode >= ISR_EVENT_COUNT) return;
		uint16_t n = _isrCounts[eventCode];
		if (n != 0xFFFF) _isrCounts[eventCode] = static_cast<uint16_t>(n + 1);
	}

	void init() {
		if (headerValid(_log.h)) {
			_pre = (_log.h.head != _log.h.tail) ? PreReset::START : PreReset::NONE;
		} else {
			// First power-up / power loss: RAM contains random content
			memset(&_log.h, 0, sizeof(_log.h));
			_log.h.magic = RING_MAGIC;
			seal();
			_pre = PreReset::NONE;
		}
		_sent = _log.h.tail;
		_bootPos = _log.h.head;
	}

	void banner() {
		logForce(Level::INFO, "sys", "MIDIControl base fw %s rev %s (%s %s)", git::branch.c_str(), git::revision.c_str(), git::commit_date.c_str(),
				 git::commit_time.c_str());
		uint32_t csr = resetcause::readAndClear();
		logForce(Level::INFO, "sys", "reset: %s (csr 0x%08lx)", resetcause::describe(csr), static_cast<unsigned long>(csr));
		// Saved fault record: a new one in full at level E (the PC counts it once), an already announced one only with a pointer to "dump"
		if (const fault::Record *r = fault::saved()) {
			if (fault::announced()) {
				logForce(Level::INFO, "fault", "last %s #%u kept (dump)", fault::kindName(r->kind), static_cast<unsigned>(r->count));
			} else {
				char text[MAX_TEXT + 1];
				for (uint8_t i = 0; fault::formatLine(*r, i, text, sizeof(text)); ++i) logForce(Level::ERROR, "fault", "%s", text);
				fault::markAnnounced();
			}
		}
	}

	Level getLevel() { return _level; }
	void setLevel(Level level) { _level = level; }

	// ── Commands from the log CDC ─────────────────────────────────────────

	static void reply(const char *text) {
		int n = snprintf(_reply, sizeof(_reply), "%s\r\n", text);
		_replyLen = (n > 0 && static_cast<size_t>(n) < sizeof(_reply)) ? static_cast<uint8_t>(n) : 0;
	}

	// Next line of the "dump" output into _reply (responses are lines without the log prefix)
	static void nextDumpLine() {
		const fault::Record *r = fault::saved();
		char text[sizeof(_reply) - 2];
		if (_dumpIdx == DUMP_NONE || !r || !fault::formatLine(*r, _dumpIdx, text, sizeof(text))) {
			_dumpIdx = DUMP_NONE;
			return;
		}
		_dumpIdx++;
		reply(text);
	}

	static void handleCommand(char *cmd) {
		// Trim spaces, lowercase
		while (*cmd == ' ') cmd++;
		size_t len = strlen(cmd);
		while (len && cmd[len - 1] == ' ') cmd[--len] = '\0';
		for (size_t i = 0; i < len; ++i) {
			if (cmd[i] >= 'A' && cmd[i] <= 'Z') cmd[i] = static_cast<char>(cmd[i] - 'A' + 'a');
		}
		if (len == 0) return;

		if (strncmp(cmd, "level", 5) == 0 && (cmd[5] == '\0' || cmd[5] == ' ')) {
			const char *arg = cmd + 5;
			while (*arg == ' ') arg++;
			if (*arg != '\0') {
				if (arg[1] != '\0') return reply("?");
				switch (*arg) {
					case 'e': _level = Level::ERROR; break;
					case 'w': _level = Level::WARNING; break;
					case 'i': _level = Level::INFO; break;
					case 'd': _level = Level::DEBUG; break;
					default: return reply("?");
				}
			}
			char text[12];
			snprintf(text, sizeof(text), "level %c", levelLetter(_level));
			return reply(text);
		}
		if (strcmp(cmd, "dump") == 0) {
			if (!fault::saved()) return reply("no fault record");
			_dumpIdx = 0;
			return nextDumpLine();
		}
		if (strcmp(cmd, "help") == 0) return reply("commands: level e|w|i|d, dump, help");
		reply("?");
	}

	static void readInput() {
		// Next command only after the response to the previous one has been sent (the rest of the input waits in the TinyUSB FIFO)
		while (_replyLen == 0 && _dumpIdx == DUMP_NONE && tud_cdc_n_available(LOG_CDC)) {
			uint8_t c;
			if (tud_cdc_n_read(LOG_CDC, &c, 1) != 1) break;
			if (c == '\r' || c == '\n') {
				_cmd[_cmdLen] = '\0';
				_cmdLen = 0;
				handleCommand(_cmd);
			} else if (_cmdLen < sizeof(_cmd) - 1) {
				_cmd[_cmdLen++] = static_cast<char>(c);
			}
		}
	}

	// ── Events from ISRs → lines ──────────────────────────────────────────

	static void reportIsrEvents() {
		static const struct {
			const char *tag;
			Level lvl;
			const char *text;
		} names[ISR_EVENT_COUNT] = {
			{"ble", Level::WARNING, "uart error"},
			{"ble", Level::WARNING, "rx ring overflow"},
			{"ble", Level::WARNING, "status queue overflow"},
			{"midi", Level::WARNING, "din uart error"},
			{"midi", Level::WARNING, "din->usb queue overflow"},
			{"sys", Level::ERROR, "log called from ISR"},
		};
		uint32_t now = stmcpp::systick::getTicks();
		if (now - _lastIsrCheck < 500) return; // at most 2 lines per second per event
		_lastIsrCheck = now;
		for (uint8_t i = 0; i < ISR_EVENT_COUNT; ++i) {
			if (_isrCounts[i] == 0) continue;
			uint32_t primask = __get_PRIMASK();
			__disable_irq();
			uint16_t n = _isrCounts[i];
			_isrCounts[i] = 0;
			__set_PRIMASK(primask);
			log(names[i].lvl, names[i].tag, "%s (%u)", names[i].text, static_cast<unsigned>(n));
		}
	}

	// ── Sending to CDC 1 ──────────────────────────────────────────────────

	// The whole text or nothing (the TX FIFO always holds only whole lines)
	static bool writeWhole(const char *text, uint32_t len, uint32_t &avail) {
		if (avail < len) return false;
		tud_cdc_n_write(LOG_CDC, text, len);
		avail -= len;
		return true;
	}

	static void drain() {
		uint32_t avail = tud_cdc_n_write_available(LOG_CDC);
		bool wrote = false;

		while (_replyLen) {
			if (!writeWhole(_reply, _replyLen, avail)) {
				if (wrote) tud_cdc_n_write_flush(LOG_CDC);
				return;
			}
			_replyLen = 0;
			wrote = true;
			nextDumpLine(); // "dump": next line of the record (or nothing)
		}

		Header &h = _log.h;
		while (true) {
			if (_pre == PreReset::START) {
				if (!before(_sent, _bootPos)) {
					_pre = PreReset::DONE; // the log before the reset has been overwritten entirely in the meantime
				} else {
					static const char marker[] = "--- log before reset ---\r\n";
					if (!writeWhole(marker, sizeof(marker) - 1, avail)) break;
					_pre = PreReset::BODY;
					wrote = true;
				}
				continue;
			}
			if (h.dropped) {
				char marker[24];
				int n = snprintf(marker, sizeof(marker), "[dropped %lu]\r\n", static_cast<unsigned long>(h.dropped));
				if (!writeWhole(marker, static_cast<uint32_t>(n), avail)) break;
				h.dropped = 0;
				seal();
				wrote = true;
				continue;
			}
			if (_pre == PreReset::BODY && !before(_sent, _bootPos)) {
				static const char marker[] = "--- end of log before reset ---\r\n";
				if (!writeWhole(marker, sizeof(marker) - 1, avail)) break;
				_pre = PreReset::DONE;
				wrote = true;
				continue;
			}
			if (_sent == h.head) break;

			// One whole line starting at _sent (up to and including '\n')
			uint32_t end = _sent;
			while (end != h.head && at(end) != '\n') end++;
			if (end == h.head) break; // incomplete line (should not happen)
			uint32_t len = end + 1 - _sent;
			if (avail < len) break; // FIFO full: continue on the next pass through the loop
			uint32_t idx = _sent % RING_SIZE;
			uint32_t first = (idx + len <= RING_SIZE) ? len : RING_SIZE - idx;
			tud_cdc_n_write(LOG_CDC, &_log.ring[idx], first);
			if (first < len) tud_cdc_n_write(LOG_CDC, &_log.ring[0], len - first);
			avail -= len;
			_sent += len;
			wrote = true;
		}

		if (wrote) tud_cdc_n_write_flush(LOG_CDC);
	}

	void process() {
		reportIsrEvents();
		if (!tud_mounted()) return;
		readInput();
		// Without an open port (DTR) everything stays in the ring; the oldest lines get overwritten ([dropped N])
		bool dtr = tud_cdc_n_connected(LOG_CDC);
		if (dtr != _dtr) {
			_dtr = dtr;
			_dtrSince = stmcpp::systick::getTicks();
			// The remainder in the TX FIFO would be discarded by the PC (flush) on the next open — start with an empty FIFO.
			// At most the FIFO contents (CFG_TUD_CDC_TX_BUFSIZE) that had not gone out by the time of closing are lost.
			tud_cdc_n_write_clear(LOG_CDC);
		}
		if (dtr && (stmcpp::systick::getTicks() - _dtrSince) >= HOLD_OFF_MS) drain();
	}

}
