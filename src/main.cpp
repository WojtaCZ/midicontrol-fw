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

// Auto-reconnect stav (neblokující: příkazy provádí automat v Bluetooth::process)
static bool _reconnectNeeded = true;
static duration _lastReconnectAttempt = 0_ms;
static constexpr duration RECONNECT_INTERVAL = 5000_ms;
// Po %CONNECT% z auto-reconnectu: zabezpečit (B) až po %STREAM_OPEN%
static bool _secureOnStreamOpen = false;

// ── Párování s ovladačem (spouští menu "Sparovat ovladac" → Bluetooth::startPairing) ──
// SCANNING:   reklama se jménem REMOTE_NAME → X, C,<typ>,<adresa>, CONNECTING (ostatní zařízení se ignorují)
// CONNECTING: %CONNECT% → B (bez dotazu na OLED, párování spustil uživatel), BONDING
// BONDING:    %SECURED% / %BONDED% → DONE
// Timeout PAIRING_TIMEOUT v kterémkoli neukončeném stavu, odpojení nebo %ERR_SEC% → FAILED.
// Auto-reconnect během párování neběží.
static void pairingFinished(bool ok, Bluetooth::PairingState failedIn) {
	using PS = Bluetooth::PairingState;
	Bluetooth::setPairingState(ok ? PS::DONE : PS::FAILED);
	if (ok) {
		Debug::log(Debug::Level::INFO, "ble", "pairing done, remote %s", Bluetooth::pendingConnectAddr());
		_reconnectNeeded = false;
		// Adresa ovladače trvale do nastavení: jen s ní se po %CONNECT% zabezpečí spojení (i po resetu base)
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
	// Uklidit podle toho, kde automat uvízl: X = konec skenování, Z = zrušit pokus o připojení, K,1 = odpojit
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
extern void signalError();

extern Oled::OledBuffer frameBuffer;

static uint8_t ledSubCounter = 0;

stmcpp::scheduler::Scheduler ledScheduler(50_ms, [](){
	LED::updateStatus();
	LED::update();

	// Odesílání stavu displeje každých 500ms (každý 10. tick)
	if (++ledSubCounter >= 10) {
		ledSubCounter = 0;
		display::sendState();
	}
}, true, false);

// Callback: OLED menu požaduje seznam písní z PC (s offsetem pro paginaci).
// Base je jen další žadatel: posílá dest=PC, SONG_LIST_RESP přijde zpět na Base.
static void onRequestSongList(uint8_t offset) {
	protocol::Packet pkt = {};
	pkt.dest = protocol::ADDR_PC;
	pkt.msgType = protocol::MsgType::GET_SONG_LIST;
	pkt.payload[0] = offset;
	pkt.payloadLen = 1;
	router::sendReliable(pkt);
}

// Callback: uživatel vybral píseň ze seznamu → PLAY_SONG na PC (ASCII kód bez terminátoru).
// Obrazovku "Prehravani" otevře až SIGNAL_PLAYING od PC.
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

// Callback: uživatel stiskl encoder na "Now Playing" → STOP_SONG na PC.
// Remote se o zastavení dozví od PC (SIGNAL_STOPPED).
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

extern "C" void SysTick_Handler(void){
	stmcpp::systick::increment();
}


extern "C" int main(void) {
	stmcpp::systick::enable(144_MHz, 1_ms);

	// Log v no-init RAM: ověřit hlavičku (platná → zachovat log z doby před resetem). Výchozí úroveň I.
	Debug::init();

	// Načtení nastavení z flash (před LED a GUI)
	settings::init();

	//Initialize the OLED
	Oled::init();
	GUI::init();
	GUI::setStopCallback(onUserStopPlayback);
	GUI::setRequestSongListCallback(onRequestSongList);
	GUI::setPlaySongCallback(onPlaySongSelected);

	// Splash hned na displej (před blokující inicializací BLE); 3 s se počítají od jeho zobrazení
	GUI::render();
	Oled::flush(500);
	splashDismissScheduler.start();
	midi::init();

	LED::init();
	ledScheduler.start(); // Start the LED scheduler

	//Initialize bluetooth
	if(Bluetooth::init()){
		Debug::log(Debug::Level::ERROR, "ble", "RN4870 init failed (no %%REBOOT%%)");
		signalError();
	}
	
	//Initialize LED display
	display::init();

	NVIC_EnableIRQ(USB_LP_IRQn);
	NVIC_EnableIRQ(USB_HP_IRQn);

	// CRS: dolaďování HSI48 podle USB SOF (registry CR/CFGR, hodiny CRS povoleny v base::initClock).
	// Pozn.: USB běží z PLLQ (HSE 48 MHz), HSI48 je tedy jen záloha.
	CRS->CFGR = (CRS->CFGR & ~CRS_CFGR_SYNCSRC) | (0b10 << CRS_CFGR_SYNCSRC_Pos);
	CRS->CR |= CRS_CR_AUTOTRIMEN;
	CRS->CR |= CRS_CR_CEN;

	usb::init();

	// Úvodní řádky logu: firmware, revize, příčina resetu (RCC->CSR se tím smaže)
	Debug::banner();

	// Konfigurace RN4870 se jen zařadí; provádí ji Bluetooth::process() bez blokování smyčky
	if(Bluetooth::configure()){
		signalError();
	}

	uint8_t packet[4];

	while (1) {

		tud_task();

		// USB → DIN (varhany)
		while (tud_midi_available() && tud_midi_packet_read(&packet[0])) {
			midi::send(packet);
			LED::notifyActivity(LED::PIXEL_MIDIB);
		}

		// DIN → USB a SysEx ozvěna displeje (TinyUSB jen z hlavní smyčky)
		midi::process();

		// Displej: nejvýše jeden rámec najednou
		display::process();

		// BLE: příjem, automat příkazového režimu
		Bluetooth::process();

		// Směrování paketů PC ↔ Remote, ACK/opakování, keepalive, odesílání CDC
		router::process();

		// Log → CDC 1 (jen s otevřeným portem), příkazy z logovacího portu
		Debug::process();

		keypressScheduler.dispatch();
		guiRenderScheduler.dispatch();
		Oled::process();
		splashDismissScheduler.dispatch();
		ledScheduler.dispatch();
		songListTimeoutScheduler.dispatch();
		songListDelayScheduler.dispatch();

		pairingTimeout();

		// Auto-reconnect k bondovanému Remote zařízení (neblokující); během párování ne
		if (_reconnectNeeded && !Bluetooth::isPairing() && !Bluetooth::isConnected() && Bluetooth::isIdle()
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
							if (Bluetooth::stopScan() && Bluetooth::connectTo(static_cast<uint8_t>(adv->getConnected()), adv->getAddr().c_str())) {
								Bluetooth::setPairingState(PS::CONNECTING);
							} else {
								// Plná fronta příkazů: X/C neodešly, nečekat 15 s na timeout
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
					Bluetooth::setBonded(true);
					if (pairing == PS::BONDING) pairingFinished(true, pairing);
					break;

				case Bluetooth::Command::Type::ERR_SEC:
					Debug::log(Debug::Level::WARNING, "ble", "ERR_SEC");
					if (pairing == PS::BONDING) {
						if (!Bluetooth::queueCommand("K,1")) Debug::log(Debug::Level::WARNING, "ble", "K,1 not queued");
						pairingFinished(false, pairing);
					}
					break;

				case Bluetooth::Command::Type::CONNECT: {
					// %CONNECT,<typ adresy 0/1>,<MAC>% — první parametr je typ adresy, ne stav spojení
					const std::string &addr = static_cast<Bluetooth::ConnectCommand *>(command.get())->getAddr();
					Debug::log(Debug::Level::INFO, "ble", "connect %s", addr.c_str());
					Bluetooth::setConnected(true);
					Bluetooth::setBonded(false);
					_reconnectNeeded = false;
					if (pairing == PS::CONNECTING) {
						if (addr == Bluetooth::pendingConnectAddr()) {
							// Párování spustil uživatel z menu → zabezpečit a uložit bond hned (bez dotazu na OLED)
							if (Bluetooth::queueCommand("B")) {
								Bluetooth::setPairingState(PS::BONDING);
							} else {
								Debug::log(Debug::Level::WARNING, "ble", "pairing: B not queued");
								Bluetooth::queueCommand("K,1");
								pairingFinished(false, pairing);
							}
						} else {
							// Během čekání na ovladač se připojil někdo jiný → odpojit, párování selhalo
							Debug::log(Debug::Level::WARNING, "ble", "pairing: foreign connect %s", addr.c_str());
							Bluetooth::queueCommand("K,1");
							pairingFinished(false, pairing);
						}
					} else if (!Bluetooth::isPairing()) {
						const char *remote = settings::remoteAddr();
						if (remote[0] != '\0' && addr == remote) {
							// Spárovaný ovladač (typicky náš auto-reconnect "C"). Spojení se samo nezabezpečí
							// (DS50002466, příkaz B: po ztrátě spojení je nutné znovu B; na lavici bez B žádný
							// %SECURED% a Remote ukázal dotaz "Sparovat?"). B až po %STREAM_OPEN%: B hned po
							// %CONNECT% skončilo na lavici %DISCONNECT%.
							_secureOnStreamOpen = true;
						} else {
							// Cizí zařízení (telefon, PC): bond vzniká jen párováním z menu → odpojit, žádný B
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
					if (_secureOnStreamOpen) {
						// B s uloženým klíčem → %SECURED% za ~0,2 s, bez dotazu na OLED
						_secureOnStreamOpen = false;
						if (!Bluetooth::queueCommand("B")) Debug::log(Debug::Level::WARNING, "ble", "B not queued, link unsecured");
					}
					break;

				case Bluetooth::Command::Type::DISCONNECT:
					Debug::log(Debug::Level::INFO, "ble", "disconnect");
					_secureOnStreamOpen = false;
					Bluetooth::setConnected(false);
					_reconnectNeeded = true;
					if (pairing == PS::CONNECTING || pairing == PS::BONDING) pairingFinished(false, pairing);
					GUI::renderForce();
					break;

				case Bluetooth::Command::Type::REBOOT:
					Debug::log(Debug::Level::WARNING, "ble", "module reboot");
					Bluetooth::setConnected(false);
					_reconnectNeeded = true;
					if (Bluetooth::isPairing()) pairingFinished(false, pairing);
					break;

				default:
					// Ostatní stavové zprávy jen do debug logu (nikdy do CDC — ten nese protokol)
					Debug::log(Debug::Level::DEBUG, "ble", "status type %u", static_cast<unsigned>(command->getType()));
					break;
			}
		}
	}

}

void signalError() {
	while (1) {
		LED::frontStrip.setColor(LED::Color(255, 0, 0, 0.5)); // Set front strip to red
		LED::update();
		stmcpp::systick::waitBlocking(500_ms);
		LED::frontStrip.setColor(LED::Color(0, 0, 0, 0.0)); // Turn off front strip
		LED::update();
		stmcpp::systick::waitBlocking(500_ms);
	}
}

void stmcpp::error::globalFaultHandler(std::uint32_t hash, std::uint32_t code) {
	//There has been an error caused by the handler, try to figure out what happened
	switch (hash) {
	
		default:
			__ASM volatile("bkpt");
			break;
	}
}



extern "C" void HardFault_Handler(void) {
	__disable_irq();

	__asm__("bkpt");
	
}


extern "C" void NMI_Handler(void) {
	__disable_irq();

	__asm__("bkpt");
	
}

extern "C" void MemManage_Handler(void) {
	__disable_irq();

	__asm__("bkpt");
	
}


extern "C" void BusFault_Handler(void) {
	__disable_irq();

	__asm__("bkpt");
	
}


extern "C" void UsageFault_Handler(void) {
	__disable_irq();

	__asm__("bkpt");
	
}
