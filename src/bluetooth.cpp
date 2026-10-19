#include "bluetooth.hpp"
#include "debug.hpp"
#include "led.hpp"
#include "ringbuffer.hpp"

#include <stdio.h>
#include <string.h>
#include <ctype.h>
#include <stdlib.h>

#include <stm32g431xx.h>
#include <core_cm4.h>
#include <cstdint>
#include <cmsis_compiler.h>

/*
USART 2 - RN4870
PA2 - TX
PA3 - RX
PA0 - MODE
PA1 - RST
RTS/CTS nejsou zapojené → hardwarové řízení toku musí být v modulu vypnuté (SR bez bitu 0x8000).
*/
using namespace std;
using namespace stmcpp::units;

namespace Bluetooth{

	Mode _mode = Mode::DATA;

	// Stav spojení; testy (tools/basetest) je čtou přes sondu podle tabulky symbolů — proto used/volatile
	[[gnu::used]] volatile bool _connected = false;
	[[gnu::used]] volatile bool _bonded = false;
	[[gnu::used]] volatile bool _streamOpen = false;
	[[gnu::used]] volatile PairingState _pairingState = PairingState::IDLE;
	static duration _pairingStart = 0_ms;
	duration _dataReceivedTimestamp = 0_ms;

	// ── Příjem (ISR → hlavní smyčka), bez haldy ──────────────────────────
	// Stavová zpráva modulu ohraničená '%'. Výsledky skenování mají až ~80 znaků
	// (adresa, typ, jméno, 128bit UUID, RSSI), proto 96.
	struct StatusLine {
		char text[96];
	};

	static RingBuffer<StatusLine, 8> _statusQueue;
	static RingBuffer<uint8_t, 512> _rxRing; // ~44 ms při 115200 Bd
	static StatusLine _rxStatus = {};
	static uint8_t _rxStatusIndex = 0;
	static bool _receivingStatus = false;
	static volatile bool _rebootSeen = false;

	// Přijaté protokolové pakety (plní process())
	static RingBuffer<protocol::Packet, 4> _rxPackets;
	static protocol::Parser _protocolParser;
	static bool _inFrame = false;

	// ── Vysílání (hlavní smyčka → DMA) ───────────────────────────────────
	static RingBuffer<uint8_t, 1024> _txRing;
	static volatile size_t _txDmaLen = 0;

	// ── Odpovědi příkazového režimu ──────────────────────────────────────
	// Posuvné okno posledních přijatých znaků mimo protokolové rámce
	static char _respWindow[5] = {};
	static bool _sawPrompt = false; // "CMD> "
	static bool _sawAok = false;
	static bool _sawErr = false;
	static bool _sawEnd = false;    // odpověď na "---"
	// "Trying" (C) a "Scanning" (F): modul za ně prompt "CMD> " neposílá, jsou to celé odpovědi
	static bool _sawProgress = false;

	// ── Neblokující automat příkazového režimu ───────────────────────────
	static constexpr uint8_t MAX_JOB_COMMANDS = 8;
	static constexpr uint8_t MAX_COMMAND_LEN = 32;
	static constexpr duration CMD_TIMEOUT = 1000_ms;
	static constexpr duration EXIT_TIMEOUT = 500_ms;
	static constexpr duration REBOOT_TIMEOUT = 3000_ms;

	enum class EngineState : uint8_t {
		IDLE,
		ENTER_CMD,   // odesláno "$$$", čekáme na "CMD> "
		SEND,        // odeslat další příkaz
		WAIT_RESP,   // čekáme na "CMD> " (případně AOK/ERR před ním)
		WAIT_REBOOT, // po "R,1" čekáme na %REBOOT%
		EXIT_CMD,    // odesláno "---", čekáme na "END"
	};

	static char _jobCommands[MAX_JOB_COMMANDS][MAX_COMMAND_LEN];
	static uint8_t _jobCount = 0;
	static uint8_t _jobIndex = 0;
	static EngineState _engineState = EngineState::IDLE;
	static duration _engineTimestamp = 0_ms;

	// Spustí DMA nad souvislým úsekem TX bufferu (volat se zakázaným přerušením DMA nebo z ISR)
	static void _kickTx() {
		if (_txDmaLen != 0) return; // DMA právě běží
		size_t len = 0;
		const uint8_t *data = _txRing.contiguousData(len);
		if (len == 0) return;

		stmcpp::reg::clear(std::ref(DMA1_Channel2->CCR), DMA_CCR_EN);
		stmcpp::reg::write(std::ref(DMA1_Channel2->CMAR), reinterpret_cast<uint32_t>(data));
		stmcpp::reg::write(std::ref(DMA1_Channel2->CNDTR), static_cast<uint32_t>(len));
		_txDmaLen = len;
		stmcpp::reg::set(std::ref(DMA1_Channel2->CCR), DMA_CCR_EN);
	}

	// Zařadí celý blok do TX bufferu (vše, nebo nic) a případně spustí DMA
	static bool _sendBytes(const uint8_t *data, size_t len) {
		if (len == 0 || _txRing.freeSpace() < len) return false;
		for (size_t i = 0; i < len; ++i) {
			_txRing.push(data[i]);
		}

		uint32_t primask = __get_PRIMASK();
		__disable_irq();
		_kickTx();
		__set_PRIMASK(primask);
		return true;
	}

	static bool _sendString(const char *str) {
		return _sendBytes(reinterpret_cast<const uint8_t *>(str), strlen(str));
	}

	// Vyhodnocení textových odpovědí modulu (AOK, ERR, CMD>, END) bez strcmp nad neukončeným bufferem
	static void _matchResponse(uint8_t c) {
		memmove(_respWindow, _respWindow + 1, sizeof(_respWindow) - 1);
		_respWindow[sizeof(_respWindow) - 1] = static_cast<char>(c);

		auto endsWith = [](const char *suffix, size_t len) { return memcmp(_respWindow + sizeof(_respWindow) - len, suffix, len) == 0; };

		if (endsWith("CMD> ", 5)) {
			_sawPrompt = true;
			_mode = Mode::COMMAND;
		} else if (endsWith("AOK", 3)) {
			_sawAok = true;
		} else if (endsWith("ERR", 3)) {
			_sawErr = true;
		} else if (endsWith("END", 3)) {
			_sawEnd = true;
		} else if (endsWith("rying", 5) || endsWith("nning", 5)) {
			_sawProgress = true;
		}
	}

	static void _clearResponseFlags() {
		_sawPrompt = false;
		_sawAok = false;
		_sawErr = false;
		_sawEnd = false;
		_sawProgress = false;
	}

	static bool _queueJob(const char *const *commands, uint8_t count) {
		if (_jobCount + count > MAX_JOB_COMMANDS) return false;
		for (uint8_t i = 0; i < count; ++i) {
			strncpy(_jobCommands[_jobCount], commands[i], MAX_COMMAND_LEN - 1);
			_jobCommands[_jobCount][MAX_COMMAND_LEN - 1] = '\0';
			_jobCount++;
		}
		return true;
	}

	static void _finishJob() {
		_jobCount = 0;
		_jobIndex = 0;
		_engineState = EngineState::IDLE;
	}

	// Konec relace příkazového režimu: příkazy od indexu `keepFrom` (zařazené až během EXIT_CMD / WAIT_REBOOT, kdy
	// je už automat neposílá) se posunou na začátek a provede je další relace "$$$" — nesmí se ztratit.
	static void _finishJobKeepQueued(uint8_t keepFrom) {
		uint8_t remaining = (keepFrom < _jobCount) ? static_cast<uint8_t>(_jobCount - keepFrom) : 0;
		for (uint8_t i = 0; i < remaining; ++i) {
			memcpy(_jobCommands[i], _jobCommands[keepFrom + i], MAX_COMMAND_LEN);
		}
		_jobCount = remaining;
		_jobIndex = 0;
		_engineState = EngineState::IDLE;
	}

	static void _runEngine() {
		duration now = stmcpp::systick::getDuration();

		switch (_engineState) {
			case EngineState::IDLE:
				if (_jobCount == 0) break;
				_jobIndex = 0;
				_clearResponseFlags();
				if (_mode == Mode::COMMAND) {
					_engineState = EngineState::SEND;
				} else {
					// "$$$" se nesmí smíchat s protokolovým rámcem — DMA musí mít prázdnou frontu
					if (!_txRing.empty()) break;
					_sendString("$$$");
					_engineTimestamp = now;
					_engineState = EngineState::ENTER_CMD;
				}
				break;

			case EngineState::ENTER_CMD:
				if (_sawPrompt) {
					_engineState = EngineState::SEND;
				} else if ((now - _engineTimestamp) > CMD_TIMEOUT) {
					// Modul nepřešel do příkazového režimu — zahodit úlohu
					Debug::log(Debug::Level::WARNING, "BLE: $$$ timeout");
					_finishJob();
				}
				break;

			case EngineState::SEND: {
				char line[MAX_COMMAND_LEN + 1];
				const char *cmd = _jobCommands[_jobIndex];
				snprintf(line, sizeof(line), "%s\r", cmd);
				_clearResponseFlags();
				_rebootSeen = false;
				if (!_sendString(line)) break; // TX fronta plná, zkusit příště
				_engineTimestamp = now;
				_engineState = (strcmp(cmd, "R,1") == 0) ? EngineState::WAIT_REBOOT : EngineState::WAIT_RESP;
				break;
			}

			case EngineState::WAIT_RESP:
				if (_sawPrompt || _sawProgress || (now - _engineTimestamp) > CMD_TIMEOUT) {
					if (_sawErr || !(_sawPrompt || _sawProgress)) {
						// Samotný příkaz ("C", "Z") by v logu nic neříkal — doplnit kontext
						char msg[MAX_COMMAND_LEN + 16];
						snprintf(msg, sizeof(msg), "BLE cmd failed: %s", _jobCommands[_jobIndex]);
						Debug::log(Debug::Level::WARNING, msg);
					}
					_jobIndex++;
					if (_jobIndex < _jobCount) {
						_engineState = EngineState::SEND;
					} else {
						// Návrat do datového režimu
						_clearResponseFlags();
						_sendString("---\r");
						_engineTimestamp = now;
						_engineState = EngineState::EXIT_CMD;
					}
				}
				break;

			case EngineState::WAIT_REBOOT:
				// Po restartu je modul v datovém režimu; zbytek úlohy se zahazuje
				if (_rebootSeen || (now - _engineTimestamp) > REBOOT_TIMEOUT) {
					_mode = Mode::DATA;
					_connected = false;
					_streamOpen = false;
					// Zbytek této úlohy za R,1 se zahazuje; co se zařadilo až po R,1, zůstává
					_finishJobKeepQueued(static_cast<uint8_t>(_jobIndex + 1));
				}
				break;

			case EngineState::EXIT_CMD:
				if (_sawEnd || (now - _engineTimestamp) > EXIT_TIMEOUT) {
					_mode = Mode::DATA;
					// Příkazy zařazené během čekání na END (např. X + C po reklamě, B po %CONNECT%) → další relace
					_finishJobKeepQueued(_jobIndex);
				}
				break;
		}
	}

	static std::unique_ptr<Command> _build_command_from_string(const char *rxCmd) {

		// --- Simple (non-parameterized) commands ---
		if      (!strcmp(rxCmd, "ADV_TIMEOUT"))      return make_unique<Command>(Command::Type::ADV_TIMEOUT);
		else if (!strcmp(rxCmd, "BONDED"))           return make_unique<Command>(Command::Type::BONDED);
		else if (!strcmp(rxCmd, "DISCONNECT"))       return make_unique<Command>(Command::Type::DISCONNECT);
		else if (!strcmp(rxCmd, "ERR_CONNPARAM"))    return make_unique<Command>(Command::Type::ERR_CONNPARAM);
		else if (!strcmp(rxCmd, "ERR_MEMORY"))       return make_unique<Command>(Command::Type::ERR_MEMORY);
		else if (!strcmp(rxCmd, "ERR_READ"))         return make_unique<Command>(Command::Type::ERR_READ);
		else if (!strcmp(rxCmd, "ERR_RMT_CMD"))      return make_unique<Command>(Command::Type::ERR_RMT_CMD);
		else if (!strcmp(rxCmd, "ERR_SEC"))          return make_unique<Command>(Command::Type::ERR_SEC);
		else if (!strcmp(rxCmd, "KEY_REQ"))          return make_unique<Command>(Command::Type::KEY_REQ);
		else if (!strcmp(rxCmd, "PIO1H"))            return make_unique<Command>(Command::Type::PIO1H);
		else if (!strcmp(rxCmd, "PIO1L"))            return make_unique<Command>(Command::Type::PIO1L);
		else if (!strcmp(rxCmd, "PIO2H"))            return make_unique<Command>(Command::Type::PIO2H);
		else if (!strcmp(rxCmd, "PIO2L"))            return make_unique<Command>(Command::Type::PIO2L);
		else if (!strcmp(rxCmd, "PIO3H"))            return make_unique<Command>(Command::Type::PIO3H);
		else if (!strcmp(rxCmd, "PIO3L"))            return make_unique<Command>(Command::Type::PIO3L);
		else if (!strcmp(rxCmd, "RE_DISCV"))         return make_unique<Command>(Command::Type::RE_DISCV);
		else if (!strcmp(rxCmd, "REBOOT"))           return make_unique<Command>(Command::Type::REBOOT);
		else if (!strcmp(rxCmd, "RMT_CMD_OFF"))      return make_unique<Command>(Command::Type::RMT_CMD_OFF);
		else if (!strcmp(rxCmd, "RMT_CMD_ON"))       return make_unique<Command>(Command::Type::RMT_CMD_ON);
		else if (!strcmp(rxCmd, "SECURED"))          return make_unique<Command>(Command::Type::SECURED);
		else if (!strcmp(rxCmd, "STREAM_OPEN"))      return make_unique<Command>(Command::Type::STREAM_OPEN);
		else if (!strcmp(rxCmd, "TMR1"))             return make_unique<Command>(Command::Type::TMR1);
		else if (!strcmp(rxCmd, "TMR2"))             return make_unique<Command>(Command::Type::TMR2);
		else if (!strcmp(rxCmd, "TMR3"))             return make_unique<Command>(Command::Type::TMR3);

		// --- Parameterized messages ---
		else if (strncmp(rxCmd, "CONN_PARAM,", 10) == 0) {
			int interval, latency, timeout;
			if (sscanf(rxCmd, "CONN_PARAM,%d,%d,%d", &interval, &latency, &timeout) == 3)
				return make_unique<ConnParamCommand>(interval, latency, timeout);
		}
		else if (strncmp(rxCmd, "CONNECT", 7) == 0) {
			int connected; char addr[32];
			if (sscanf(rxCmd, "CONNECT,%d,%31[^%%]", &connected, addr) == 2)
				return make_unique<ConnectCommand>(connected, addr);
		}
		else if (strncmp(rxCmd, "KEY:", 4) == 0) {
			int key;
			if (sscanf(rxCmd, "KEY:%d", &key) == 1)
				return make_unique<KeyCommand>(key);
		}
		else if (strncmp(rxCmd, "INDI,", 4) == 0) {
			int hdl; char hex[128];
			if (sscanf(rxCmd, "INDI,%d,%127[^%%]", &hdl, hex) == 2)
				return make_unique<IndiCommand>(hdl, hex);
		}
		else if (strncmp(rxCmd, "NOTI,", 5) == 0) {
			int hdl; char hex[128];
			if (sscanf(rxCmd, "NOTI,%d,%127[^%%]", &hdl, hex) == 2)
				return make_unique<NotiCommand>(hdl, hex);
		}
		else if (strncmp(rxCmd, "RV,", 3) == 0) {
			int hdl; char hex[128];
			if (sscanf(rxCmd, "RV,%d,%127[^%%]", &hdl, hex) == 2)
				return make_unique<RvCommand>(hdl, hex);
		}
		else if (strncmp(rxCmd, "S_RUN:", 6) == 0) {
			char cmd[64];
			if (sscanf(rxCmd, "S_RUN:%63[^%%]", cmd) == 1)
				return make_unique<SRunCommand>(cmd);
		}
		else if (strncmp(rxCmd, "WC,", 3) == 0) {
			int hdl; char hex[128];
			if (sscanf(rxCmd, "WC,%d,%127[^%%]", &hdl, hex) == 2)
				return make_unique<WcCommand>(hdl, hex);
		}
		else if (strncmp(rxCmd, "WV,", 3) == 0) {
			int hdl; char hex[128];
			if (sscanf(rxCmd, "WV,%d,%127[^%%]", &hdl, hex) == 2)
				return make_unique<WvCommand>(hdl, hex);
		}

		else if (!strcmp(rxCmd, "ERR_CONN")) return make_unique<Command>(Command::Type::ERR_CONN);

		// --- Výsledky skenování (F) ---
		// Připojitelná:    <adresa 12 hex>,<typ 0/1>,<jméno>,<UUID>,<RSSI hex>   (jméno i UUID mohou být prázdné)
		// Nepřipojitelná:  <adresa 12 hex>,<typ 0/1>,<RSSI hex>,Brcst:<data hex>
		// Na lavici např. "D88039FE5996,0,MIDIControlRemote,,F5" a "CAD46E49E827,1,AD,Brcst:07FF4C0012020001".
		else {
			const char *field[5] = {};
			size_t fieldLen[5] = {};
			uint8_t count = 0;
			const char *p = rxCmd;
			while (count < 5) {
				const char *comma = (count < 4) ? strchr(p, ',') : nullptr;
				field[count] = p;
				fieldLen[count] = comma ? static_cast<size_t>(comma - p) : strlen(p);
				count++;
				if (!comma) break;
				p = comma + 1;
			}

			auto isHex = [](const char *f, size_t len) {
				if (len == 0) return false;
				for (size_t i = 0; i < len; ++i) {
					if (!isxdigit(static_cast<unsigned char>(f[i]))) return false;
				}
				return true;
			};
			auto toStr = [](const char *f, size_t len) { return std::string(f, len); };

			bool addrOk = count >= 4 && fieldLen[0] == 12 && isHex(field[0], 12) && fieldLen[1] == 1 && (field[1][0] == '0' || field[1][0] == '1');
			if (addrOk) {
				int type = field[1][0] - '0';
				if (count == 4 && strncmp(field[3], "Brcst:", 6) == 0 && isHex(field[2], fieldLen[2])) {
					int rssi = static_cast<int8_t>(strtol(toStr(field[2], fieldLen[2]).c_str(), nullptr, 16));
					return make_unique<AdvNonConnectableCommand>(toStr(field[0], 12), type, rssi, std::string(field[3] + 6));
				}
				if (count == 5 && isHex(field[4], fieldLen[4])) {
					int rssi = static_cast<int8_t>(strtol(field[4], nullptr, 16));
					return make_unique<AdvConnectableCommand>(toStr(field[0], 12), type, toStr(field[2], fieldLen[2]), toStr(field[3], fieldLen[3]), rssi);
				}
			}
		}

		// If no match was found, return an 'UNKNOWN' command
		return make_unique<Command>(Command::Type::UNKNOWN);
	}


	uint8_t init(){
		// USART GPIOs
		stmcpp::gpio::pin<stmcpp::gpio::port::porta, 2> usart_tx (stmcpp::gpio::mode::af7, stmcpp::gpio::otype::pushPull, stmcpp::gpio::speed::low, stmcpp::gpio::pull::noPull);
		stmcpp::gpio::pin<stmcpp::gpio::port::porta, 3> usart_rx (stmcpp::gpio::mode::af7);

		// RN 487x control pins
        stmcpp::gpio::pin<stmcpp::gpio::port::porta, 0> mode (stmcpp::gpio::mode::output, stmcpp::gpio::otype::pushPull, stmcpp::gpio::speed::low, stmcpp::gpio::pull::noPull);
		stmcpp::gpio::pin<stmcpp::gpio::port::porta, 1> rst_n (stmcpp::gpio::mode::output, stmcpp::gpio::otype::pushPull, stmcpp::gpio::speed::low, stmcpp::gpio::pull::noPull);
		
		// Go into application mode
		mode.set();

		// Reset the RN487x
		rst_n.clear();

		// Set up TX dma
        stmcpp::reg::write(std::ref(DMA1_Channel2->CCR), 
            (0b1 << DMA_CCR_MINC_Pos) | // Memory increment mode
			(0b1 << DMA_CCR_DIR_Pos)   | // Read from memory
            (0b1 << DMA_CCR_TCIE_Pos)   // Transfer complete interrupt enable
        );

        // Set up the DMA peripheral (adresa paměti se nastavuje při každém spuštění v _kickTx)
        stmcpp::reg::write(std::ref(DMA1_Channel2->CPAR), reinterpret_cast<uint32_t>(&USART2->TDR));
        NVIC_EnableIRQ(DMA1_Channel2_IRQn);

        // Set up DMAMUX routing (DMAMUX channels start from 0, DMA from 1); 27 = USART2_TX
        stmcpp::reg::write(std::ref(DMAMUX1_Channel1->CCR), (27 << DMAMUX_CxCR_DMAREQ_ID_Pos));

		// Set up USART2
		stmcpp::reg::write(std::ref(USART2->BRR), 144000000 / 115200); // PCLK1 = 144 MHz, 115200 Bd
		stmcpp::reg::write(std::ref(USART2->CR1), 
			(0b1 << USART_CR1_TE_Pos) | // Transmitter enable
			(0b1 << USART_CR1_RE_Pos) | // Receiver enable
			(0b1 << USART_CR1_RXNEIE_Pos)  // RX not empty interrupt enable
		);

		stmcpp::reg::write(std::ref(USART2->CR3), 
			(0b1 << USART_CR3_DMAT_Pos)    // DMA transmitter enable
		);

		NVIC_EnableIRQ(USART2_IRQn);

		// Enable USART2
		stmcpp::reg::set(std::ref(USART2->CR1), USART_CR1_UE);

		stmcpp::systick::waitBlocking(100_ms);

		// Bring RN487x out of reset
		rst_n.set();

		// Jednorázové čekání na %REBOOT% při startu (před spuštěním hlavní smyčky)
		duration timestamp = stmcpp::systick::getDuration();
		while(!isCommandAvailable() && stmcpp::systick::getDuration() < (timestamp + DEFAULT_TIMEOUT)){;}

		std::unique_ptr<Bluetooth::Command> command = Bluetooth::getCommand();
		if(command && command->getType() == Bluetooth::Command::Type::REBOOT){
			_mode = Mode::DATA;
			return 0; // Successful init after reboot
		}else return 1;
	}

	uint8_t configure() {
		// SA,2: NoInputNoOutput (Just Works) — Base nemá klávesnici ani nezobrazuje passkey,
		//       spárování potvrzuje uživatel dotazem na OLED (B / K,1).
		// SR,0000: 16bit bitmapa funkcí; bez řízení toku (0x8000), RTS/CTS nejsou zapojené.
		//          Prompt "CMD> " zůstává zapnutý, automat na něj čeká.
		// SS,C0: Device Information + UART Transparent.
		// R,1: restart, aby se změny projevily; po %REBOOT% je modul v datovém režimu.
		static const char *const commands[] = {"SN,MIDIControlBase", "SA,2", "SR,0000", "SS,C0", "R,1"};
		return _queueJob(commands, sizeof(commands) / sizeof(commands[0])) ? 0 : 1;
	}

	uint8_t reconnect() {
		if (_engineState != EngineState::IDLE || _jobCount != 0) return 1;
		// Z: zrušit případný probíhající pokus (ERR, když žádný neběží — nevadí)
		// C: připojit se k poslednímu bondovanému zařízení (link se automaticky zabezpečí)
		static const char *const commands[] = {"Z", "C"};
		return _queueJob(commands, 2) ? 0 : 1;
	}

	// noinline + used: testy ji volají přes gdb (Release ELF bez DWARF, symbol musí zůstat)
	[[gnu::used, gnu::noinline]] bool queueCommand(const char *command) {
		const char *const commands[] = {command};
		return _queueJob(commands, 1);
	}

	bool isPairing() {
		PairingState s = _pairingState;
		return s == PairingState::SCANNING || s == PairingState::CONNECTING || s == PairingState::BONDING;
	}

	[[gnu::used, gnu::noinline]] uint8_t startPairing() {
		if (isPairing()) return 1;
		// Při spojení nejdřív odpojit (K,1) — Remote během spojení neadvertisuje.
		// Z: zrušit probíhající pokus o připojení (auto-reconnect "C"), jinak modul nic nenaskenuje.
		// F: skenování (výsledky chodí jako %...% i v datovém režimu, každé zařízení jednou).
		static const char *const withDisconnect[] = {"K,1", "Z", "F"};
		const char *const *commands = _connected ? withDisconnect : withDisconnect + 1;
		uint8_t count = _connected ? 3 : 2;
		if (!_queueJob(commands, count)) return 1;
		_pairingStart = stmcpp::systick::getDuration();
		_pairingState = PairingState::SCANNING;
		return 0;
	}

	bool stopScan() { return queueCommand("X"); }

	static char _connectAddr[13] = "";

	const char *pendingConnectAddr() { return _connectAddr; }

	bool connectTo(uint8_t addrType, const char *addr) {
		strncpy(_connectAddr, addr, sizeof(_connectAddr) - 1);
		_connectAddr[sizeof(_connectAddr) - 1] = '\0';
		char cmd[MAX_COMMAND_LEN];
		snprintf(cmd, sizeof(cmd), "C,%u,%s", static_cast<unsigned>(addrType), addr);
		return queueCommand(cmd);
	}

	PairingState pairingState() { return _pairingState; }
	void setPairingState(PairingState state) { _pairingState = state; }
	duration pairingElapsed() { return stmcpp::systick::getDuration() - _pairingStart; }

	// noinline: bezpečné místo hlavní smyčky pro zastavení testy (dbg.Target.call)
	[[gnu::noinline]] void process() {
		uint8_t c;
		// Dokud je místo ve frontě paketů; zbytek zůstane v RX bufferu na příště
		while (_rxPackets.freeSpace() > 0 && _rxRing.pop(c)) {
			if (c == protocol::FRAME_START) _inFrame = true;

			if (_inFrame) {
				if (_protocolParser.feed(c)) {
					_rxPackets.push(_protocolParser.getPacket());
				}
				if (c == protocol::FRAME_END) _inFrame = false;
			} else {
				_matchResponse(c);
			}
		}

		_runEngine();
	}

	bool isIdle() { return _engineState == EngineState::IDLE && _jobCount == 0 && _mode == Mode::DATA; }

	bool isConnected() { return _connected; }
	void setConnected(bool connected) {
		_connected = connected;
		if (!connected) _streamOpen = false;
	}
	bool isBonded() { return _bonded; }
	void setBonded(bool bonded) { _bonded = bonded; }
	bool isStreamOpen() { return _streamOpen; }
	void setStreamOpen(bool open) { _streamOpen = open; }

	bool canSendData() { return _connected && _streamOpen && isIdle(); }

	duration getDataReceivedTimestamp() { return _dataReceivedTimestamp; }

	bool isCommandAvailable() {
		return !_statusQueue.empty();
	}

	std::unique_ptr<Command> getCommand() {
		StatusLine line;
		if (!_statusQueue.pop(line)) {
			return nullptr; // No command string available
		}
		line.text[sizeof(line.text) - 1] = '\0';
		return _build_command_from_string(line.text);
	}

	bool isProtocolPacketAvailable() {
		return !_rxPackets.empty();
	}

	bool getProtocolPacket(protocol::Packet &pkt) {
		return _rxPackets.pop(pkt);
	}

	bool sendProtocolPacket(const protocol::Packet &pkt) {
		if (!canSendData()) return false;

		uint8_t frame[protocol::MAX_FRAME_SIZE];
		uint16_t len = protocol::encode(pkt, frame);
		if (len == 0 || len > protocol::MAX_FRAME_SIZE) return false;
		if (!_sendBytes(frame, len)) return false;

		LED::notifyActivity(LED::PIXEL_BLUETOOTH);
		return true;
	}

	extern "C" void DMA1_Channel2_IRQHandler(){
		if(stmcpp::reg::read(std::ref(DMA1->ISR)) & DMA_ISR_TCIF2){
			stmcpp::reg::set(std::ref(DMA1->IFCR), DMA_IFCR_CTCIF2);
			stmcpp::reg::clear(std::ref(DMA1_Channel2->CCR), DMA_CCR_EN);

			// Uvolnit odeslaný úsek a pokračovat dalším
			_txRing.consume(_txDmaLen);
			_txDmaLen = 0;
			_kickTx();
        }
	}

	extern "C" void USART2_IRQHandler() {
		uint32_t isr = stmcpp::reg::read(std::ref(USART2->ISR));

		if (isr & (USART_ISR_ORE | USART_ISR_FE | USART_ISR_NE)) {
			stmcpp::reg::write(std::ref(USART2->ICR), USART_ICR_ORECF | USART_ICR_FECF | USART_ICR_NECF);
		}

		if(isr & USART_ISR_RXNE){
			char c = static_cast<char>(USART2->RDR);
			if (!_receivingStatus && c == STATUS_DELIMITER){
				// Začátek stavové zprávy
				_receivingStatus = true;
				_rxStatusIndex = 0;
			}else if (_receivingStatus && c == STATUS_DELIMITER){
				// Konec stavové zprávy → fronta (bez haldy)
				_rxStatus.text[_rxStatusIndex] = '\0';
				_receivingStatus = false;
				if (strcmp(_rxStatus.text, "REBOOT") == 0) _rebootSeen = true;
				_statusQueue.push(_rxStatus);
				_rxStatusIndex = 0;
			}else if (_receivingStatus){
				// Příliš dlouhé zprávy (např. výsledky skenování) se zkrátí
				if (_rxStatusIndex < sizeof(_rxStatus.text) - 1) {
					_rxStatus.text[_rxStatusIndex++] = c;
				}
			}else{
				// Data (protokolové rámce i textové odpovědi) zpracuje hlavní smyčka
				_dataReceivedTimestamp = stmcpp::systick::getDuration();
				_rxRing.push(static_cast<uint8_t>(c));
			}
        }
	}
}
