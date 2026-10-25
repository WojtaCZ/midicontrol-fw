#include "bluetooth.hpp"
#include "wdg.hpp"
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
RTS/CTS are not wired → hardware flow control must be disabled in the module (SR without bit 0x8000).
*/
using namespace std;
using namespace stmcpp::units;

namespace Bluetooth{

	Mode _mode = Mode::DATA;

	// Connection state; tests (tools/basetest) read it through the probe via the symbol table — hence used/volatile
	[[gnu::used]] volatile bool _connected = false;
	[[gnu::used]] volatile bool _bonded = false;
	[[gnu::used]] volatile bool _streamOpen = false;
	[[gnu::used]] volatile PairingState _pairingState = PairingState::IDLE;
	// Accepted peer: link secured (%SECURED%/%BONDED%) with a device whose address from %CONNECT% matched
	// the stored remote (or the pairing address). Without it received frames are dropped and nothing is sent.
	[[gnu::used]] volatile bool _peerAccepted = false;
	static bool _dropLogged = false; // one log line about dropped frames per connection
	static duration _pairingStart = 0_ms;
	duration _dataReceivedTimestamp = 0_ms;

	// ── Receive (ISR → main loop), no heap ───────────────────────────────
	// Module status message delimited by '%'. Scan results are up to ~80 characters
	// (address, type, name, 128-bit UUID, RSSI), hence 96.
	struct StatusLine {
		char text[96];
	};

	static RingBuffer<StatusLine, 8> _statusQueue;
	static RingBuffer<uint8_t, 512> _rxRing; // ~44 ms at 115200 Bd
	static StatusLine _rxStatus = {};
	static uint8_t _rxStatusIndex = 0;
	static bool _receivingStatus = false;
	static volatile bool _rebootSeen = false;

	// Received protocol packets (filled by process())
	static RingBuffer<protocol::Packet, 4> _rxPackets;
	static protocol::Parser _protocolParser;
	static bool _inFrame = false;

	// ── Transmit (main loop → DMA) ───────────────────────────────────────
	static RingBuffer<uint8_t, 1024> _txRing;
	static volatile size_t _txDmaLen = 0;

	// ── Command mode responses ───────────────────────────────────────────
	// Sliding window of the last received characters outside protocol frames
	static char _respWindow[5] = {};
	static bool _sawPrompt = false; // "CMD> "
	static bool _sawAok = false;
	static bool _sawErr = false;
	static bool _sawEnd = false;    // response to "---"
	// "Trying" (C) a "Scanning" (F): the module does not send the "CMD> " prompt after them, they are complete responses
	static bool _sawProgress = false;

	// ── Non-blocking command mode state machine ──────────────────────────
	static constexpr uint8_t MAX_JOB_COMMANDS = 8;
	static constexpr uint8_t MAX_COMMAND_LEN = 32;
	static constexpr duration CMD_TIMEOUT = 1000_ms;
	static constexpr duration EXIT_TIMEOUT = 500_ms;
	static constexpr duration REBOOT_TIMEOUT = 3000_ms;

	enum class EngineState : uint8_t {
		IDLE,
		ENTER_CMD,   // "$$$" sent, waiting for "CMD> "
		SEND,        // send the next command
		WAIT_RESP,   // waiting for "CMD> " (possibly AOK/ERR before it)
		WAIT_REBOOT, // after "R,1" waiting for %REBOOT%
		EXIT_CMD,    // "---" sent, waiting for "END"
	};

	static char _jobCommands[MAX_JOB_COMMANDS][MAX_COMMAND_LEN];
	static uint8_t _jobCount = 0;
	static uint8_t _jobIndex = 0;
	static EngineState _engineState = EngineState::IDLE;
	static duration _engineTimestamp = 0_ms;

	// Configuration and health state of the module (read by the main loop): configuration is done only with %REBOOT% after "R,1"
	static bool _configJob = false;
	static ConfigState _configState = ConfigState::NONE;
	static uint8_t _dollarTimeouts = 0; // consecutive "$$$" timeouts (module is silent)
	static constexpr uint8_t WEDGED_TIMEOUTS = 3;

	// Non-blocking module restart via RST (PA1, active low): RST low 100 ms → high → wait for %REBOOT% ≤ 1 s
	static RestartState _restartState = RestartState::IDLE;
	static duration _restartTimestamp = 0_ms;
	static bool _restartRstHigh = false;
	static constexpr duration RESTART_RST_LOW = 100_ms;
	static bool _moduleUp = false;
	static void _runRestart();

	// Starts DMA over a contiguous section of the TX buffer (call with the DMA interrupt disabled or from the ISR)
	static void _kickTx() {
		if (_txDmaLen != 0) return; // DMA is running right now
		size_t len = 0;
		const uint8_t *data = _txRing.contiguousData(len);
		if (len == 0) return;

		stmcpp::reg::clear(std::ref(DMA1_Channel2->CCR), DMA_CCR_EN);
		stmcpp::reg::write(std::ref(DMA1_Channel2->CMAR), reinterpret_cast<uint32_t>(data));
		stmcpp::reg::write(std::ref(DMA1_Channel2->CNDTR), static_cast<uint32_t>(len));
		_txDmaLen = len;
		stmcpp::reg::set(std::ref(DMA1_Channel2->CCR), DMA_CCR_EN);
	}

	// Queues the whole block into the TX buffer (all or nothing) and starts DMA if needed
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

	// Evaluation of the module's text responses (AOK, ERR, CMD>, END) without strcmp over an unterminated buffer
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

	// End of a command mode session: commands from index `keepFrom` (queued only during EXIT_CMD / WAIT_REBOOT, when
	// the state machine no longer sends them) are moved to the start and executed by the next "$$$" session — they must not be lost.
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
					// "$$$" must not mix with a protocol frame — the DMA queue must be empty
					if (!_txRing.empty()) break;
					_sendString("$$$");
					_engineTimestamp = now;
					_engineState = EngineState::ENTER_CMD;
				}
				break;

			case EngineState::ENTER_CMD:
				if (_sawPrompt) {
					_dollarTimeouts = 0;
					_engineState = EngineState::SEND;
				} else if ((now - _engineTimestamp) > CMD_TIMEOUT) {
					// The module did not enter command mode — drop the job
					Debug::log(Debug::Level::WARNING, "ble", "$$$ timeout");
					if (_dollarTimeouts < 255) _dollarTimeouts++;
					if (_configJob) { _configJob = false; _configState = ConfigState::FAILED; }
					_finishJob();
				}
				break;

			case EngineState::SEND: {
				char line[MAX_COMMAND_LEN + 1];
				const char *cmd = _jobCommands[_jobIndex];
				snprintf(line, sizeof(line), "%s\r", cmd);
				_clearResponseFlags();
				_rebootSeen = false;
				if (!_sendString(line)) break; // TX queue full, try next time
				Debug::log(Debug::Level::DEBUG, "ble", "cmd %s", cmd);
				_engineTimestamp = now;
				_engineState = (strcmp(cmd, "R,1") == 0) ? EngineState::WAIT_REBOOT : EngineState::WAIT_RESP;
				break;
			}

			case EngineState::WAIT_RESP:
				if (_sawPrompt || _sawProgress || (now - _engineTimestamp) > CMD_TIMEOUT) {
					if (_sawErr || !(_sawPrompt || _sawProgress)) {
						// The bare command ("C", "Z") would say nothing in the log — add context
						Debug::log(Debug::Level::WARNING, "ble", "cmd failed: %s", _jobCommands[_jobIndex]);
					}
					_jobIndex++;
					if (_jobIndex < _jobCount) {
						_engineState = EngineState::SEND;
					} else {
						// Return to data mode
						_clearResponseFlags();
						_sendString("---\r");
						_engineTimestamp = now;
						_engineState = EngineState::EXIT_CMD;
					}
				}
				break;

			case EngineState::WAIT_REBOOT:
				// After the restart the module is in data mode; the rest of the job is discarded
				if (_rebootSeen || (now - _engineTimestamp) > REBOOT_TIMEOUT) {
					if (_configJob) {
						_configJob = false;
						_configState = _rebootSeen ? ConfigState::DONE : ConfigState::FAILED;
					}
					_mode = Mode::DATA;
					_connected = false;
					_streamOpen = false;
					resetPeer();
					// The rest of this job after R,1 is discarded; whatever was queued after R,1 stays
					_finishJobKeepQueued(static_cast<uint8_t>(_jobIndex + 1));
				}
				break;

			case EngineState::EXIT_CMD:
				if (_sawEnd || (now - _engineTimestamp) > EXIT_TIMEOUT) {
					_mode = Mode::DATA;
					// Commands queued while waiting for END (e.g. X + C after advertising, B after %CONNECT%) → next session
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

		// --- Scan results (F) ---
		// Connectable:      <address 12 hex>,<type 0/1>,<name>,<UUID>,<RSSI hex>   (name and UUID may be empty)
		// Non-connectable:  <address 12 hex>,<type 0/1>,<RSSI hex>,Brcst:<data hex>
		// On the bench e.g. "D88039FE5996,0,MIDIControlRemote,,F5" and "CAD46E49E827,1,AD,Brcst:07FF4C0012020001".
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

        // Set up the DMA peripheral (the memory address is set on every start in _kickTx)
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

		wdg::kick();
		stmcpp::systick::waitBlocking(100_ms);

		// Bring RN487x out of reset
		rst_n.set();

		// One-time wait for %REBOOT% at startup (before the main loop starts)
		duration timestamp = stmcpp::systick::getDuration();
		// Up to ~1.1 s together with the 100 ms above: kick, otherwise the loop watchdog (1000 ms) would record LOOP_STALL
		while(!isCommandAvailable() && stmcpp::systick::getDuration() < (timestamp + DEFAULT_TIMEOUT)){ wdg::kick(); }

		std::unique_ptr<Bluetooth::Command> command = Bluetooth::getCommand();
		if(command && command->getType() == Bluetooth::Command::Type::REBOOT){
			_mode = Mode::DATA;
			return 0; // Successful init after reboot
		}else return 1;
	}

	// noinline + used: test_fault.py calls it via gdb (reconfiguration under the watchdog)
	static constexpr const char *NAME_COMMAND = "SN,MIDIControlBase";

	const char *name() { return NAME_COMMAND + 3; } // after "SN,"

	[[gnu::used, gnu::noinline]] uint8_t configure() {
		// SA,2: NoInputNoOutput (Just Works) — the Base has no keyboard and does not display a passkey; a bond is created only
		//       by pairing from the menu (B after %CONNECT% from the found remote), without an OLED prompt.
		// SR,0000: 16-bit feature bitmap; no flow control (0x8000), RTS/CTS are not wired.
		//          The "CMD> " prompt stays enabled, the state machine waits for it.
		// SS,C0: Device Information + UART Transparent.
		// R,1: restart so the changes take effect; after %REBOOT% the module is in data mode.
		static const char *const commands[] = {NAME_COMMAND, "SA,2", "SR,0000", "SS,C0", "R,1"};
		if (!_queueJob(commands, sizeof(commands) / sizeof(commands[0]))) return 1;
		_configJob = true;
		_configState = ConfigState::PENDING;
		return 0;
	}

	uint8_t reconnect() {
		if (_engineState != EngineState::IDLE || _jobCount != 0) return 1;
		// Z: cancel a possible attempt in progress (ERR when none is running — harmless)
		// C: connect to the last bonded device (the link is secured automatically)
		static const char *const commands[] = {"Z", "C"};
		return _queueJob(commands, 2) ? 0 : 1;
	}

	// noinline + used: tests call it via gdb (Release ELF without DWARF, the symbol must remain)
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
		// If connected, disconnect first (K,1) — the Remote does not advertise while connected.
		// Z: cancel a connection attempt in progress (auto-reconnect "C"), otherwise the module scans nothing.
		// F: scan (results arrive as %...% even in data mode, each device once).
		static const char *const withDisconnect[] = {"K,1", "Z", "F"};
		const char *const *commands = _connected ? withDisconnect : withDisconnect + 1;
		uint8_t count = _connected ? 3 : 2;
		if (!_queueJob(commands, count)) return 1;
		_pairingStart = stmcpp::systick::getDuration();
		_pairingState = PairingState::SCANNING;
		Debug::log(Debug::Level::INFO, "ble", "pairing started");
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

	// noinline: a safe place in the main loop for tests to halt at (dbg.Target.call)
	[[gnu::noinline]] void process() {
		uint8_t c;
		// While there is room in the packet queue; the rest stays in the RX buffer for next time
		while (_rxPackets.freeSpace() > 0 && _rxRing.pop(c)) {
			if (c == protocol::FRAME_START) _inFrame = true;

			if (_inFrame) {
				if (_protocolParser.feed(c)) {
					if (_peerAccepted) {
						_rxPackets.push(_protocolParser.getPacket());
					} else if (!_dropLogged) {
						// Unsecured or foreign peer: the frame is not executed (the Remote retries after %SECURED%)
						_dropLogged = true;
						Debug::log(Debug::Level::INFO, "ble", "frame from unaccepted peer dropped");
					}
				}
				if (c == protocol::FRAME_END) _inFrame = false;
			} else {
				_matchResponse(c);
			}
		}

		_runRestart();
		_runEngine();
	}

	static void _runRestart() {
		if (_restartState != RestartState::RUNNING) return;
		duration now = stmcpp::systick::getDuration();
		if (!_restartRstHigh) {
			if ((now - _restartTimestamp) < RESTART_RST_LOW) return;
			GPIOA->BSRR = (1u << 1); // RST high
			_rebootSeen = false;
			_restartRstHigh = true;
			_restartTimestamp = now;
		} else if (_rebootSeen) {
			_mode = Mode::DATA;
			_restartState = RestartState::SUCCEEDED;
		} else if ((now - _restartTimestamp) > DEFAULT_TIMEOUT) {
			_restartState = RestartState::FAILED;
		}
	}

	void restart() {
		// Discard commands in progress and the connection state; USART/DMA stay as set up by init()
		_finishJob();
		_configJob = false;
		_configState = ConfigState::NONE;
		_dollarTimeouts = 0;
		_mode = Mode::DATA;
		_connected = false;
		_streamOpen = false;
		_bonded = false;
		resetPeer();
		GPIOA->BSRR = (1u << (1 + 16)); // RST low
		_restartRstHigh = false;
		_restartTimestamp = stmcpp::systick::getDuration();
		_restartState = RestartState::RUNNING;
	}
	RestartState restartState() { return _restartState; }
	void restartAck() { _restartState = RestartState::IDLE; }
	ConfigState configState() { return _configState; }
	bool wedged() { return _dollarTimeouts >= WEDGED_TIMEOUTS; }
	bool isUp() { return _moduleUp; }
	void setUp(bool up) { _moduleUp = up; }

	bool isIdle() { return _engineState == EngineState::IDLE && _jobCount == 0 && _mode == Mode::DATA; }

	bool isConnected() { return _connected; }
	void setConnected(bool connected) {
		_connected = connected;
		if (!connected) {
			_streamOpen = false;
			resetPeer();
		}
	}
	bool isBonded() { return _bonded; }
	void setBonded(bool bonded) { _bonded = bonded; }
	bool isStreamOpen() { return _streamOpen; }
	void setStreamOpen(bool open) { _streamOpen = open; }

	bool canSendData() { return _connected && _streamOpen && _peerAccepted && isIdle(); }

	bool isPeerAccepted() { return _peerAccepted; }
	void setPeerAccepted() { _peerAccepted = true; }

	void resetPeer() {
		_peerAccepted = false;
		_dropLogged = false;
		_rxPackets.clear();
		_protocolParser.reset();
		_inFrame = false;
	}

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
		// Safeguard: without an accepted peer do not execute anything from the queue
		if (!_peerAccepted) {
			_rxPackets.clear();
			return false;
		}
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

			// Release the sent section and continue with the next one
			_txRing.consume(_txDmaLen);
			_txDmaLen = 0;
			_kickTx();
        }
	}

	extern "C" void USART2_IRQHandler() {
		uint32_t isr = stmcpp::reg::read(std::ref(USART2->ISR));

		if (isr & (USART_ISR_ORE | USART_ISR_FE | USART_ISR_NE)) {
			stmcpp::reg::write(std::ref(USART2->ICR), USART_ICR_ORECF | USART_ICR_FECF | USART_ICR_NECF);
			Debug::logFromIsr(Debug::ISR_BLE_UART_ERROR);
		}

		if(isr & USART_ISR_RXNE){
			char c = static_cast<char>(USART2->RDR);
			if (!_receivingStatus && c == STATUS_DELIMITER){
				// Start of a status message
				_receivingStatus = true;
				_rxStatusIndex = 0;
			}else if (_receivingStatus && c == STATUS_DELIMITER){
				// End of a status message → queue (no heap)
				_rxStatus.text[_rxStatusIndex] = '\0';
				_receivingStatus = false;
				if (strcmp(_rxStatus.text, "REBOOT") == 0) _rebootSeen = true;
				if (!_statusQueue.push(_rxStatus)) Debug::logFromIsr(Debug::ISR_BLE_STATUS_OVERFLOW);
				_rxStatusIndex = 0;
			}else if (_receivingStatus){
				// Messages that are too long (e.g. scan results) are truncated
				if (_rxStatusIndex < sizeof(_rxStatus.text) - 1) {
					_rxStatus.text[_rxStatusIndex++] = c;
				}
			}else{
				// Data (protocol frames and text responses) is processed by the main loop
				_dataReceivedTimestamp = stmcpp::systick::getDuration();
				if (!_rxRing.push(static_cast<uint8_t>(c))) Debug::logFromIsr(Debug::ISR_BLE_RX_OVERFLOW);
			}
        }
	}
}
