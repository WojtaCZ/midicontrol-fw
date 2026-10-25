#include "main.hpp"
#include "oled.hpp"
#include "base.hpp"
#include "scheduler.hpp"
#include "menu.hpp"
#include "midi.hpp"
#include "led.hpp"
#include "bluetooth.hpp"
#include "display.hpp"
#include "usb.hpp"
#include "debug.hpp"
#include "settings.hpp"
#include "router.hpp"
#include "fault.hpp"
#include "wdg.hpp"

#include <cstring>

#include <stm32g431xx.h>
#include <core_cm4.h>
#include <cmsis_compiler.h>

#include <tusb.h>
#include <tinyusb/src/class/midi/midi_device.h>
#include <tinyusb/src/class/midi/midi.h>
#include <tinyusb/src/class/cdc/cdc_device.h>

#include "protocol/types.hpp"
#include "protocol/constants.hpp"

#include <gfx/text.hpp>
#include <gfx/icon.hpp>

using namespace stmcpp::units;

// Auto-reconnect state (non-blocking: commands are executed by the state machine in Bluetooth::process)
static bool _reconnectNeeded = true;
static duration _lastReconnectAttempt = 0_ms;
static constexpr duration RECONNECT_INTERVAL = 5000_ms;
// After %CONNECT% from auto-reconnect: secure (B) only after %STREAM_OPEN%
static bool _secureOnStreamOpen = false;
// The address from the last %CONNECT% belongs to the stored remote (outside pairing) or to the pairing address (CONNECTING).
// Only then does %SECURED%/%BONDED% make the peer accepted (Bluetooth::setPeerAccepted) and its frames get executed.
static bool _peerAddrMatched = false;
// Pairing: one more connection attempt after a drop. The first connection after the base lost its bond (U,Z, new base)
// while the remote still holds the old one ends in %CONNECT%%DISCONNECT%; the next attempt bonds (bench 2026-10-25).
static uint8_t _pairAddrType = 0;
static bool _pairRetried = false;

// Without a stored remote (new base, upgrade from settings v1) it connects to nothing (no "C" every 5 s)
static bool remoteKnown() { return settings::remoteAddr()[0] != '\0'; }

// ── Pairing with the remote (started by the menu "Sparovat ovladac" → Bluetooth::startPairing) ──
// SCANNING:   advertisement named REMOTE_NAME → X, C,<type>,<address>, CONNECTING (other devices are ignored)
// CONNECTING: %CONNECT% → BONDING, B after %STREAM_OPEN% (no OLED prompt, the user started the pairing)
// BONDING:    %SECURED% / %BONDED% → DONE
// PAIRING_TIMEOUT in any unfinished state, a second disconnect (the first one connects again) or %ERR_SEC% → FAILED.
// Auto-reconnect does not run during pairing.
static void pairingFinished(bool ok, Bluetooth::PairingState failedIn) {
	using PS = Bluetooth::PairingState;
	Bluetooth::setPairingState(ok ? PS::DONE : PS::FAILED);
	if (ok) {
		Debug::log(Debug::Level::INFO, "ble", "pairing done, remote %s", Bluetooth::pendingConnectAddr());
		_reconnectNeeded = false;
		// Store the remote's address permanently in the settings: only with it is the link secured after %CONNECT% (even after a base reset)
		if (strcmp(settings::remoteAddr(), Bluetooth::pendingConnectAddr()) != 0) {
			settings::setRemoteAddr(Bluetooth::pendingConnectAddr());
			settings::save();
		}
		GUI::showPairingResult("Sparovano");
	} else {
		Debug::log(Debug::Level::WARNING, "ble", "pairing failed in state %u", static_cast<unsigned>(failedIn));
		_reconnectNeeded = true;
		_lastReconnectAttempt = stmcpp::systick::getDuration();
		GUI::showPairingResult(failedIn == PS::SCANNING ? "Ovladac nenalezen" : "Parovani selhalo");
	}
}

static void pairingTimeout() {
	using PS = Bluetooth::PairingState;
	PS state = Bluetooth::pairingState();
	if (!Bluetooth::isPairing() || Bluetooth::pairingElapsed() < Bluetooth::PAIRING_TIMEOUT) return;
	// Clean up depending on where the state machine got stuck: X = end scan, Z = cancel connection attempt, K,1 = disconnect
	bool queued = true;
	if (state == PS::SCANNING) queued &= Bluetooth::stopScan();
	if (state == PS::CONNECTING) queued &= Bluetooth::queueCommand("Z");
	if (Bluetooth::isConnected() || state == PS::BONDING) queued &= Bluetooth::queueCommand("K,1");
	if (!queued) Debug::log(Debug::Level::WARNING, "ble", "pairing: cleanup not queued");
	Debug::log(Debug::Level::WARNING, "ble", "pairing timeout (state %u)", static_cast<unsigned>(state));
	pairingFinished(false, state);
}

extern stmcpp::scheduler::Scheduler oledSleepScheduler;
extern stmcpp::scheduler::Scheduler keypressScheduler;
extern stmcpp::scheduler::Scheduler guiRenderScheduler;
extern stmcpp::scheduler::Scheduler commTimeoutScheduler;
extern stmcpp::scheduler::Scheduler dispChangeScheduler;
extern stmcpp::scheduler::Scheduler songListTimeoutScheduler;
extern stmcpp::scheduler::Scheduler songListDelayScheduler;
[[noreturn]] void signalError(uint16_t code);

// RN4870 state (spec R21): failure = one ERROR record without a reset, running without BLE, next attempt after BLE_RETRY_INTERVAL.
// The restart is non-blocking (Bluetooth::restart() driven from Bluetooth::process()), the main loop does not idle for a second.
static constexpr duration BLE_RETRY_INTERVAL = 60000_ms;
enum class BleState : uint8_t { CONFIGURING, READY, DOWN, RESTARTING };
static BleState _bleState = BleState::DOWN;
static bool _bleWasDown = false; // after an outage: report the return
static bool _bleErrorRecorded = false; // ERROR record only once
static duration _bleRetryAt = 0_ms;
static void bleFailed(uint16_t code, const char *what);
static void bleBootStart();
static void bleProcess();

// A link without an accepted peer (re-review I-1): outside pairing it must not hang forever (frames are dropped, the PC gets
// NAK UNREACHABLE, the remote does not disconnect by itself). SECURE_KICK_TIMEOUT after %CONNECT%/%STREAM_OPEN% without acceptance → for the address
// of the stored remote B once more, after another timeout K,1 (then auto-reconnect in 5 s). A foreign address never gets B, only K,1.
// Pairing has its own 15 s timeout. Non-blocking: checked from the main loop (secureWatchdog), one W line per intervention.
static constexpr duration SECURE_KICK_TIMEOUT = 3000_ms;
enum class SecureWatch : uint8_t { IDLE, WAIT_SECURE, WAIT_RETRY };
static SecureWatch _secureWatch = SecureWatch::IDLE;
static duration _secureWatchSince = 0_ms;

// %CONNECT% / %STREAM_OPEN%: restart the countdown from this event (B is sent only after STREAM_OPEN)
static void secureWatchArm() {
	_secureWatch = SecureWatch::WAIT_SECURE;
	_secureWatchSince = stmcpp::systick::getDuration();
}

static void secureWatchdog() {
	if (_bleState != BleState::READY || Bluetooth::isPairing() || !Bluetooth::isConnected() || Bluetooth::isPeerAccepted()) {
		_secureWatch = SecureWatch::IDLE;
		return;
	}
	duration now = stmcpp::systick::getDuration();
	if (_secureWatch == SecureWatch::IDLE) {
		// A link without acceptance that did not start with an event (e.g. after a failed pairing) → watch it too
		_secureWatch = SecureWatch::WAIT_SECURE;
		_secureWatchSince = now;
		return;
	}
	if ((now - _secureWatchSince) <= SECURE_KICK_TIMEOUT) return;
	_secureWatchSince = now;
	if (_secureWatch == SecureWatch::WAIT_SECURE && _peerAddrMatched && Bluetooth::isStreamOpen()) {
		_secureWatch = SecureWatch::WAIT_RETRY;
		if (Bluetooth::queueCommand("B")) {
			Debug::log(Debug::Level::WARNING, "ble", "link not secured in 3 s, B again");
			return;
		}
		// B was not sent → disconnect right away (auto-reconnect will try again)
		Debug::log(Debug::Level::WARNING, "ble", "link not secured, B not queued, K,1");
	} else {
		_secureWatch = SecureWatch::WAIT_RETRY;
		Debug::log(Debug::Level::WARNING, "ble", "link not accepted, K,1");
	}
	// On failure (or when %DISCONNECT% does not arrive) K,1 is tried again after the next timeout
	if (!Bluetooth::queueCommand("K,1")) Debug::log(Debug::Level::WARNING, "ble", "K,1 not queued");
}

extern Oled::OledBuffer frameBuffer;

static uint8_t ledSubCounter = 0;

stmcpp::scheduler::Scheduler ledScheduler(50_ms, [](){
	LED::updateStatus();
	LED::update();

	// Send the display state every 500 ms (every 10th tick)
	if (++ledSubCounter >= 10) {
		ledSubCounter = 0;
		display::sendState();
	}
}, true, false);

// Callback: the OLED menu requests the song list from the PC (with an offset for pagination).
// The Base is just another requester: it sends dest=PC, SONG_LIST_RESP comes back to the Base.
static void onRequestSongList(uint8_t offset) {
	protocol::Packet pkt = {};
	pkt.dest = protocol::ADDR_PC;
	pkt.msgType = protocol::MsgType::GET_SONG_LIST;
	pkt.payload[0] = offset;
	pkt.payloadLen = 1;
	router::sendReliable(pkt);
}

// Callback: the user selected a song from the list → PLAY_SONG to the PC (ASCII code without a terminator).
// The "Prehravani" screen is opened only by SIGNAL_PLAYING from the PC.
static void onPlaySongSelected(const char* songName) {
	protocol::Packet pkt = {};
	pkt.dest = protocol::ADDR_PC;
	pkt.msgType = protocol::MsgType::PLAY_SONG;
	uint8_t len = 0;
	while (songName[len] && len < protocol::MAX_SONG_CODE_LEN) {
		pkt.payload[len] = static_cast<uint8_t>(songName[len]);
		len++;
	}
	pkt.payloadLen = len;
	router::sendReliable(pkt);
}

// Callback: the user pressed the encoder on "Now Playing" → STOP_SONG to the PC.
// The Remote learns about the stop from the PC (SIGNAL_STOPPED).
static void onUserStopPlayback() {
	protocol::Packet stopPkt = {};
	stopPkt.dest = protocol::ADDR_PC;
	stopPkt.msgType = protocol::MsgType::STOP_SONG;
	stopPkt.payloadLen = 0;
	router::sendReliable(stopPkt);
}

// One-shot scheduler: auto-dismiss the splash screen after 3 seconds
stmcpp::scheduler::Scheduler splashDismissScheduler(3000_ms, [](){
	GUI::keypress(ui::InputEvent::Enter);
}, false, false);

extern "C" void SystemInit(void) {

	// The application is linked at 0x08008000 (the first 32K hold the USB
	// bootloader). Relocate the vector table accordingly.
	SCB->VTOR = 0x08008000;

	// Enable the FPU if needed
	#if (__FPU_PRESENT == 1) && (__FPU_USED == 1)
		stmcpp::reg::set(std::ref(SCB->CPACR), (3UL << 20U) | (3UL << 22U));
    #endif

	//Initialize io and other stuff related to the base
	base::initClock();
	


}

// SysTick: tick + loop watchdog. The naked wrapper passes the frame of the interrupted context (stack according to EXC_RETURN bit 2)
// and EXC_RETURN; SysTick_C returns via bx lr = EXC_RETURN (the wrapper does not change lr).
extern "C" [[gnu::used, gnu::externally_visible]] void SysTick_C(const uint32_t *frame, uint32_t excReturn) {
	stmcpp::systick::increment();
	wdg::onTick(frame, excReturn);
}

extern "C" [[gnu::naked]] void SysTick_Handler(void) {
	__asm volatile("tst lr, #4\n\t"
				   "ite eq\n\t"
				   "mrseq r0, msp\n\t"
				   "mrsne r0, psp\n\t"
				   "mov r1, lr\n\t"
				   "b SysTick_C\n\t");
}


extern "C" int main(void) {
	stmcpp::systick::enable(144_MHz, 1_ms);

	// Log in no-init RAM: verify the header (valid → keep the log from before the reset). Default level I.
	Debug::init();
	fault::init();

	// IWDG (~2 s) + loop watchdog (1000 ms) right after the clocks, before the blocking peripheral initialization
	wdg::start();

	// Initial log lines: firmware, revision, reset cause (this clears RCC->CSR), saved fault record.
	// Before peripheral initialization: an initialization failure (signalError) is thus logged and recorded.
	Debug::banner();

	// Load the settings from flash (before LED and GUI)
	settings::init();

	//Initialize the OLED
	Oled::init();
	GUI::init();
	GUI::setStopCallback(onUserStopPlayback);
	GUI::setRequestSongListCallback(onRequestSongList);
	GUI::setPlaySongCallback(onPlaySongSelected);

	// Splash straight to the display (before the blocking BLE initialization); the 3 s are counted from when it is shown
	GUI::render();
	Oled::flush(500);
	splashDismissScheduler.start();
	midi::init();

	LED::init();
	ledScheduler.start(); // Start the LED scheduler

	//Initialize bluetooth
	// An RN4870 failure is not fatal (spec R21): the base keeps running without BLE and retries init after 60 s
	bleBootStart();
	
	//Initialize LED display
	display::init();

	NVIC_EnableIRQ(USB_LP_IRQn);
	NVIC_EnableIRQ(USB_HP_IRQn);

	// CRS: HSI48 trimming from USB SOF (CR/CFGR registers, CRS clock enabled in base::initClock).
	// Note: USB runs from PLLQ (HSE 48 MHz), so HSI48 is only a backup.
	CRS->CFGR = (CRS->CFGR & ~CRS_CFGR_SYNCSRC) | (0b10 << CRS_CFGR_SYNCSRC_Pos);
	CRS->CR |= CRS_CR_AUTOTRIMEN;
	CRS->CR |= CRS_CR_CEN;

	usb::init();

	// The RN4870 configuration is only queued; Bluetooth::process() carries it out without blocking the loop

	uint8_t packet[4];

	while (1) {

		// Loop watchdog + IWDG: one kick per pass
		wdg::kick();

		// Test fault injection (tools/basetest/test_fault.py): one read and compare per pass
		if (uint8_t inject = fault::testInject; inject != 0) {
			fault::testInject = 0;
			if (inject == 1) (void)*reinterpret_cast<volatile uint32_t *>(0xFFFFFFF0); // BusFault (precise, BFAR)
			else if (inject == 2) fault::testSpin();                                    // loop stall
			else if (inject == 3) fault::testSpinNoIrq();                               // IWDG
		}

		tud_task();

		// USB → DIN (organ)
		while (tud_midi_available() && tud_midi_packet_read(&packet[0])) {
			midi::send(packet);
			LED::notifyActivity(LED::PIXEL_USB);
			LED::notifyActivity(LED::PIXEL_MIDIB);
		}

		// DIN → USB and the SysEx echo of the display (TinyUSB only from the main loop)
		midi::process();

		// Display: at most one frame at a time
		display::process();

		// BLE: receive, command mode state machine
		Bluetooth::process();

		// Packet routing PC ↔ Remote, ACK/retry, keepalive, CDC sending
		router::process();

		// Log → CDC 1 (only with the port open), commands from the log port
		Debug::process();

		keypressScheduler.dispatch();
		guiRenderScheduler.dispatch();
		oledSleepScheduler.dispatch();
		Oled::process();
		splashDismissScheduler.dispatch();
		ledScheduler.dispatch();
		songListTimeoutScheduler.dispatch();
		songListDelayScheduler.dispatch();

		pairingTimeout();

		bleProcess();

		// A link without an accepted peer outside pairing: B once more, then K,1
		secureWatchdog();

		// Auto-reconnect to the bonded Remote device (non-blocking); not during pairing and not without a stored remote
		if (_bleState == BleState::READY && _reconnectNeeded && remoteKnown() && !Bluetooth::isPairing() && !Bluetooth::isConnected() && Bluetooth::isIdle()
			&& (stmcpp::systick::getDuration() - _lastReconnectAttempt) > RECONNECT_INTERVAL) {
			_lastReconnectAttempt = stmcpp::systick::getDuration();
			Debug::log(Debug::Level::DEBUG, "ble", "reconnect attempt");
			Bluetooth::reconnect();
		}

		while (Bluetooth::isCommandAvailable()) {
			std::unique_ptr<Bluetooth::Command> command = Bluetooth::getCommand();
			if (!command) break;

			using PS = Bluetooth::PairingState;
			PS pairing = Bluetooth::pairingState();

			switch (command->getType()) {
				case Bluetooth::Command::Type::ADV_CONNECTABLE:
					if (pairing == PS::SCANNING) {
						auto *adv = static_cast<Bluetooth::AdvConnectableCommand *>(command.get());
						if (adv->getName() == Bluetooth::REMOTE_NAME) {
							Debug::log(Debug::Level::INFO, "ble", "pairing: remote found %s", adv->getAddr().c_str());
							_pairAddrType = static_cast<uint8_t>(adv->getConnected());
							_pairRetried = false;
							if (Bluetooth::stopScan() && Bluetooth::connectTo(_pairAddrType, adv->getAddr().c_str())) {
								Bluetooth::setPairingState(PS::CONNECTING);
							} else {
								// Command queue full: X/C were not sent, do not wait 15 s for the timeout
								Debug::log(Debug::Level::WARNING, "ble", "pairing: command queue full");
								Bluetooth::stopScan();
								pairingFinished(false, pairing);
							}
						}
					}
					break;

				case Bluetooth::Command::Type::BONDED:
				case Bluetooth::Command::Type::SECURED:
					Debug::log(Debug::Level::INFO, "ble", "%s", command->getType() == Bluetooth::Command::Type::BONDED ? "bonded" : "secured");
					// Frames are executed only from a secured link with the correct address (stored remote / pairing)
					if (_peerAddrMatched && (pairing == PS::BONDING || !Bluetooth::isPairing())) {
						Bluetooth::setBonded(true);
						Bluetooth::setPeerAccepted();
						if (pairing == PS::BONDING) pairingFinished(true, pairing);
					} else {
						Debug::log(Debug::Level::WARNING, "ble", "secured link from unexpected peer, K,1");
						if (Bluetooth::isConnected() && !Bluetooth::queueCommand("K,1")) Debug::log(Debug::Level::WARNING, "ble", "K,1 not queued");
						if (pairing == PS::BONDING) pairingFinished(false, pairing);
					}
					break;

				case Bluetooth::Command::Type::ERR_SEC:
					Debug::log(Debug::Level::WARNING, "ble", "ERR_SEC");
					if (pairing == PS::BONDING) {
						if (!Bluetooth::queueCommand("K,1")) Debug::log(Debug::Level::WARNING, "ble", "K,1 not queued");
						pairingFinished(false, pairing);
					} else if (!Bluetooth::isPairing() && Bluetooth::isConnected()) {
						// Securing failed (the peer does not have our key, e.g. a spoofed address) → disconnect
						if (!Bluetooth::queueCommand("K,1")) Debug::log(Debug::Level::WARNING, "ble", "K,1 not queued");
					}
					break;

				case Bluetooth::Command::Type::CONNECT: {
					// %CONNECT,<address type 0/1>,<MAC>% — the first parameter is the address type, not the connection state
					const std::string &addr = static_cast<Bluetooth::ConnectCommand *>(command.get())->getAddr();
					Debug::log(Debug::Level::INFO, "ble", "connect %s", addr.c_str());
					// New connection: nothing from the previous one applies (peer acceptance, frame queue, B after STREAM_OPEN)
					_secureOnStreamOpen = false;
					_peerAddrMatched = false;
					Bluetooth::resetPeer();
					Bluetooth::setConnected(true);
					Bluetooth::setBonded(false);
					_reconnectNeeded = false;
					secureWatchArm();
					if (pairing == PS::SCANNING) {
						// Nobody should connect to us while scanning (we are still looking for the remote) → disconnect, the scan keeps running
						Debug::log(Debug::Level::WARNING, "ble", "pairing: unexpected connect %s, K,1", addr.c_str());
						if (!Bluetooth::queueCommand("K,1")) Debug::log(Debug::Level::WARNING, "ble", "K,1 not queued");
					} else if (pairing == PS::CONNECTING) {
						if (addr == Bluetooth::pendingConnectAddr()) {
							_peerAddrMatched = true;
							// The user started pairing from the menu → secure and store the bond (no OLED prompt). B only after
							// %STREAM_OPEN%, as on reconnect: B right after %CONNECT% made the link drop in most pairings (bench 2026-10-25)
							_secureOnStreamOpen = true;
							Bluetooth::setPairingState(PS::BONDING);
						} else {
							// Someone else connected while waiting for the remote → disconnect, pairing failed
							Debug::log(Debug::Level::WARNING, "ble", "pairing: foreign connect %s", addr.c_str());
							Bluetooth::queueCommand("K,1");
							pairingFinished(false, pairing);
						}
					} else if (!Bluetooth::isPairing()) {
						const char *remote = settings::remoteAddr();
						if (remote[0] != '\0' && addr == remote) {
							_peerAddrMatched = true;
							// Paired remote (typically our auto-reconnect "C"). The link does not secure itself
							// (DS50002466, command B: after losing the link B is needed again; on the bench without B there was no
							// %SECURED% and the Remote showed the "Sparovat?" prompt). B only after %STREAM_OPEN%: B right after
							// %CONNECT% ended in %DISCONNECT% on the bench.
							_secureOnStreamOpen = true;
						} else {
							// Foreign device (phone, PC): a bond is created only by pairing from the menu → disconnect, no B
							Debug::log(Debug::Level::WARNING, "ble", "unknown device %s, K,1", addr.c_str());
							if (!Bluetooth::queueCommand("K,1")) Debug::log(Debug::Level::WARNING, "ble", "K,1 not queued");
						}
					}
					GUI::renderForce();
					break;
				}

				case Bluetooth::Command::Type::STREAM_OPEN:
					Debug::log(Debug::Level::INFO, "ble", "stream open");
					Bluetooth::setConnected(true);
					Bluetooth::setStreamOpen(true);
					secureWatchArm();
					if (_secureOnStreamOpen) {
						// B with the stored key → %SECURED% within ~0.2 s, no OLED prompt
						_secureOnStreamOpen = false;
						if (!Bluetooth::queueCommand("B")) {
							if (pairing == PS::BONDING) {
								Debug::log(Debug::Level::WARNING, "ble", "pairing: B not queued");
								Bluetooth::queueCommand("K,1");
								pairingFinished(false, pairing);
							} else {
								Debug::log(Debug::Level::WARNING, "ble", "B not queued, retry in 3 s");
							}
						}
					}
					break;

				case Bluetooth::Command::Type::DISCONNECT:
					Debug::log(Debug::Level::INFO, "ble", "disconnect");
					_secureOnStreamOpen = false;
					_peerAddrMatched = false;
					Bluetooth::setConnected(false);
					_reconnectNeeded = true;
					if (pairing == PS::CONNECTING || pairing == PS::BONDING) {
						if (!_pairRetried && Bluetooth::connectTo(_pairAddrType, Bluetooth::pendingConnectAddr())) {
							_pairRetried = true;
							Debug::log(Debug::Level::INFO, "ble", "pairing: link dropped, connecting again");
							Bluetooth::setPairingState(PS::CONNECTING);
						} else {
							pairingFinished(false, pairing);
						}
					}
					GUI::renderForce();
					break;

				case Bluetooth::Command::Type::REBOOT:
					Debug::log(Debug::Level::WARNING, "ble", "module reboot");
					_secureOnStreamOpen = false;
					_peerAddrMatched = false;
					Bluetooth::setConnected(false);
					Bluetooth::setBonded(false);
					_reconnectNeeded = true;
					if (Bluetooth::isPairing()) pairingFinished(false, pairing);
					break;

				default:
					// Other status messages only into the debug log (never into the CDC — it carries the protocol)
					Debug::log(Debug::Level::DEBUG, "ble", "status type %u", static_cast<unsigned>(command->getType()));
					break;
			}
		}
	}

}

static void bleFailed(uint16_t code, const char *what) {
	_bleState = BleState::DOWN;
	_bleWasDown = true;
	_bleRetryAt = stmcpp::systick::getDuration();
	Bluetooth::setUp(false);
	// Dead module: the link is void (otherwise the router would "forward" into the dead module and STATUS_RESP would report ble=1)
	Bluetooth::setConnected(false);
	Bluetooth::setBonded(false);
	_secureOnStreamOpen = false;
	_peerAddrMatched = false;
	_reconnectNeeded = true;
	if (!_bleErrorRecorded) {
		_bleErrorRecorded = true;
		Debug::log(Debug::Level::ERROR, "ble", "RN4870 %s failed, running without BLE, retry every 60 s", what);
		fault::recordNonFatal(code, reinterpret_cast<uint32_t>(__builtin_return_address(0)));
	} else {
		Debug::log(Debug::Level::WARNING, "ble", "RN4870 %s failed again", what);
	}
}

// Start at boot (before USB): blocking init, then the configuration goes into the state machine
static void bleBootStart() {
	if (Bluetooth::init()) {
		bleFailed(fault::ERR_BLE_INIT, "init");
		return;
	}
	if (Bluetooth::configure()) {
		bleFailed(fault::ERR_BLE_CONFIGURE, "configure");
		return;
	}
	_bleState = BleState::CONFIGURING;
}

// Once per pass through the loop; nothing blocks
static void bleProcess() {
	using RS = Bluetooth::RestartState;
	using CS = Bluetooth::ConfigState;
	switch (_bleState) {
		case BleState::CONFIGURING:
			if (Bluetooth::configState() == CS::DONE) {
				_bleState = BleState::READY;
				Bluetooth::setUp(true);
				if (_bleWasDown) Debug::log(Debug::Level::INFO, "ble", "RN4870 init ok after retry");
				_bleWasDown = false;
			} else if (Bluetooth::configState() == CS::FAILED) {
				bleFailed(fault::ERR_BLE_CONFIGURE, "configure");
			}
			break;
		case BleState::READY:
			if (Bluetooth::wedged()) bleFailed(fault::ERR_BLE_INIT, "not responding");
			break;
		case BleState::DOWN:
			if (!Bluetooth::isPairing() && (stmcpp::systick::getDuration() - _bleRetryAt) > BLE_RETRY_INTERVAL) {
				Bluetooth::restart();
				_bleState = BleState::RESTARTING;
			}
			break;
		case BleState::RESTARTING:
			if (Bluetooth::restartState() == RS::SUCCEEDED) {
				Bluetooth::restartAck();
				if (Bluetooth::configure()) bleFailed(fault::ERR_BLE_CONFIGURE, "configure");
				else _bleState = BleState::CONFIGURING;
			} else if (Bluetooth::restartState() == RS::FAILED) {
				Bluetooth::restartAck();
				bleFailed(fault::ERR_BLE_INIT, "init");
			}
			break;
	}
}

// Wait with watchdog kicking (signalError: the blinking lasts longer than the loop watchdog)
static void waitKicking(uint32_t ms) {
	uint32_t start = stmcpp::systick::getTicks();
	while (stmcpp::systick::getTicks() - start < ms) wdg::kick();
}

// Unrecoverable error: ERROR record with a code (+ log line), ~3 s of red blinking, then reset (with a probe bkpt)
[[gnu::noinline]] void signalError(uint16_t code) {
	uint32_t caller = reinterpret_cast<uint32_t>(__builtin_return_address(0));
	Debug::log(Debug::Level::ERROR, "sys", "error %u, reset in 3 s", static_cast<unsigned>(code));
	// Pseudo-frame: r0 = code, lr = pc = call site
	const uint32_t frame[8] = {code, 0, 0, 0, 0, caller, caller, 0x01000000};
	fault::record(fault::Kind::ERROR, frame, 0, code);
	for (int i = 0; i < 3; ++i) {
		LED::frontStrip.setColor(LED::Color(255, 0, 0, 0.5));
		LED::update();
		waitKicking(500);
		LED::frontStrip.setColor(LED::Color(0, 0, 0, 0.0));
		LED::update();
		waitKicking(500);
	}
	fault::finish();
}

void stmcpp::error::globalFaultHandler(std::uint32_t hash, std::uint32_t code) {
	// Error reported by the stmcpp library: r0 = module hash, r1 = error code, lr = pc = call site
	uint32_t caller = reinterpret_cast<uint32_t>(__builtin_return_address(0));
	const uint32_t frame[8] = {hash, code, 0, 0, 0, caller, caller, 0x01000000};
	fault::capture(fault::Kind::ERROR, frame, 0, static_cast<uint16_t>(fault::ERR_STMCPP | (code & 0x7FFF)));
}

// HardFault/MemManage/BusFault/UsageFault/NMI: naked stubs in fault.cpp → fault::capture
