#include "debug.hpp"
#include "resetcause.hpp"
#include "git.hpp"

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

	// ── Ring v no-init RAM (0x20007800, NOINIT v linker skriptu aplikace i bootloaderu) ──────────────
	// head/tail jsou absolutní pozice (stále rostou), index = pozice % RING_SIZE. Ring drží posledních
	// RING_SIZE bajtů historie bez ohledu na to, co už odešlo do CDC: po resetu se celá vypíše jako
	// "log before reset". Co se přepíše dřív, než to odešlo, se počítá v `dropped` (řádky).
	static constexpr uint32_t RING_MAGIC = 0x4C4F4721; // "!GOL"
	static constexpr uint32_t RING_SIZE = 1536;
	static constexpr uint8_t LOG_CDC = 1; // TinyUSB instance log CDC
	static constexpr size_t MAX_TEXT = 120;

	struct Header {
		uint32_t magic;
		uint32_t head;
		uint32_t tail;
		uint32_t dropped;
		uint32_t crc; // CRC-32 předchozích 4 polí
	};

	struct NoInitLog {
		Header h;
		char ring[RING_SIZE];
	};

	// used: testy (dbg.debug_log) ho čtou sondou podle tabulky symbolů
	[[gnu::section(".noinit.log"), gnu::used]] NoInitLog _log;

	static Level _level = Level::INFO;
	static uint32_t _sent = 0;    // absolutní pozice: co už odešlo do CDC (tail <= _sent <= head)
	static uint32_t _bootPos = 0; // head v okamžiku init(): hranice logu před resetem

	// Stav výpisu logu před resetem
	enum class PreReset : uint8_t { NONE, START, BODY, DONE };
	static PreReset _pre = PreReset::NONE;

	// Port otevřen (DTR) a od kdy: první řádky se posílají až po HOLD_OFF_MS, protože PC po otevření portu maže
	// vstupní buffer (pyserial i termios tcflush) a co přijde hned po DTR, by se ztratilo
	static constexpr uint32_t HOLD_OFF_MS = 200;
	static bool _dtr = false;
	static uint32_t _dtrSince = 0;

	static volatile uint16_t _isrCounts[ISR_EVENT_COUNT] = {};
	static uint32_t _lastIsrCheck = 0;

	// Odpověď na příkaz (posílá se přednostně, mimo ring)
	static char _reply[96];
	static uint8_t _replyLen = 0;
	static char _cmd[32];
	static uint8_t _cmdLen = 0;

	// ── CRC hlavičky ──────────────────────────────────────────────────────

	static uint32_t crc32(const void *data, size_t len) {
		const uint8_t *p = static_cast<const uint8_t *>(data);
		uint32_t crc = 0xFFFFFFFF;
		for (size_t i = 0; i < len; ++i) {
			crc ^= p[i];
			for (int b = 0; b < 8; ++b) crc = (crc >> 1) ^ (0xEDB88320 & (0u - (crc & 1)));
		}
		return ~crc;
	}

	static uint32_t headerCrc(const Header &h) { return crc32(&h, offsetof(Header, crc)); }

	static void seal() { _log.h.crc = headerCrc(_log.h); }

	static bool headerValid(const Header &h) {
		return h.magic == RING_MAGIC && h.crc == headerCrc(h) && (h.head - h.tail) <= RING_SIZE;
	}

	// Porovnání absolutních pozic odolné proti přetečení uint32 (po 4 GB logu)
	static bool before(uint32_t a, uint32_t b) { return static_cast<int32_t>(a - b) < 0; }

	static char at(uint32_t pos) { return _log.ring[pos % RING_SIZE]; }

	// ── Zápis do ringu ────────────────────────────────────────────────────

	// Uvolní místo pro `need` bajtů po celých řádcích (nejstarší první)
	static void makeRoom(uint32_t need) {
		Header &h = _log.h;
		uint32_t tail = h.tail;
		uint32_t dropped = 0;
		while (h.head + need - tail > RING_SIZE) {
			uint32_t lineStart = tail;
			while (tail != h.head && at(tail) != '\n') tail++;
			if (tail == h.head) break; // bez konce řádku (nemá nastat): vše pryč
			tail++;
			if (!before(lineStart, _sent)) dropped++; // řádek neodešel
		}
		if (before(_sent, tail)) _sent = tail;
		// Nejdřív hlavička (tail), teprve potom se přepisují bajty: reset uprostřed nechá platný stav
		h.tail = tail;
		h.dropped += dropped;
		seal();
	}

	static void append(const char *data, uint32_t len) {
		if (len == 0 || len > RING_SIZE) return;
		Header &h = _log.h;
		if (h.tail >= 0x80000000u) {
			// 2^32 není násobek RING_SIZE: pozice včas posunout o násobek RING_SIZE (indexy i rozdíly zůstanou)
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
		// Řádek musí zůstat jedním řádkem
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

	// ── Veřejné API ───────────────────────────────────────────────────────

	void log(Level lvl, const char *tag, const char *fmt, ...) {
		// Z přerušení se nesmí zapisovat do ringu (hlavní smyčka může být uprostřed append): jen započítat
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

	void logFromIsr(uint8_t eventCode) {
		if (eventCode >= ISR_EVENT_COUNT) return;
		uint16_t n = _isrCounts[eventCode];
		if (n != 0xFFFF) _isrCounts[eventCode] = static_cast<uint16_t>(n + 1);
	}

	void init() {
		if (headerValid(_log.h)) {
			_pre = (_log.h.head != _log.h.tail) ? PreReset::START : PreReset::NONE;
		} else {
			// První zapnutí / výpadek napájení: v RAM je náhodný obsah
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
		// Task 6C: uložený záznam o chybě (fault record) se vypíše zde
	}

	Level getLevel() { return _level; }
	void setLevel(Level level) { _level = level; }

	// ── Příkazy z log CDC ─────────────────────────────────────────────────

	static void reply(const char *text) {
		int n = snprintf(_reply, sizeof(_reply), "%s\r\n", text);
		_replyLen = (n > 0 && static_cast<size_t>(n) < sizeof(_reply)) ? static_cast<uint8_t>(n) : 0;
	}

	static void handleCommand(char *cmd) {
		// Ořezat mezery, malá písmena
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
		if (strcmp(cmd, "dump") == 0) return reply("no fault record"); // Task 6C: výpis záznamu o chybě
		if (strcmp(cmd, "help") == 0) return reply("commands: level e|w|i|d, dump, help");
		reply("?");
	}

	static void readInput() {
		// Další příkaz až po odeslání odpovědi na předchozí (zbytek vstupu počká v TinyUSB FIFO)
		while (_replyLen == 0 && tud_cdc_n_available(LOG_CDC)) {
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

	// ── Události z ISR → řádky ────────────────────────────────────────────

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
		if (now - _lastIsrCheck < 500) return; // nejvýše 2 řádky za sekundu na událost
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

	// ── Odesílání do CDC 1 ────────────────────────────────────────────────

	// Celý text, nebo nic (v TX FIFO jsou vždy jen celé řádky)
	static bool writeWhole(const char *text, uint32_t len, uint32_t &avail) {
		if (avail < len) return false;
		tud_cdc_n_write(LOG_CDC, text, len);
		avail -= len;
		return true;
	}

	static void drain() {
		uint32_t avail = tud_cdc_n_write_available(LOG_CDC);
		bool wrote = false;

		if (_replyLen) {
			if (!writeWhole(_reply, _replyLen, avail)) return;
			_replyLen = 0;
			wrote = true;
		}

		Header &h = _log.h;
		while (true) {
			if (_pre == PreReset::START) {
				if (!before(_sent, _bootPos)) {
					_pre = PreReset::DONE; // log před resetem se mezitím celý přepsal
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

			// Jeden celý řádek od _sent (do '\n' včetně)
			uint32_t end = _sent;
			while (end != h.head && at(end) != '\n') end++;
			if (end == h.head) break; // neúplný řádek (nemá nastat)
			uint32_t len = end + 1 - _sent;
			if (avail < len) break; // FIFO plné: pokračovat při dalším průchodu smyčkou
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
		// Bez otevřeného portu (DTR) zůstává vše v ringu; nejstarší řádky se přepisují ([dropped N])
		bool dtr = tud_cdc_n_connected(LOG_CDC);
		if (dtr != _dtr) {
			_dtr = dtr;
			_dtrSince = stmcpp::systick::getTicks();
			// Zbytek v TX FIFO by po dalším otevření smazal PC (flush) — začít s prázdnou FIFO.
			// Ztratí se nejvýše obsah FIFO (CFG_TUD_CDC_TX_BUFSIZE), který při zavření ještě neodešel.
			tud_cdc_n_write_clear(LOG_CDC);
		}
		if (dtr && (stmcpp::systick::getTicks() - _dtrSince) >= HOLD_OFF_MS) drain();
	}

}
