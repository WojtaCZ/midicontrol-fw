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

#include <utility>

#include <stm32g431xx.h>
#include <core_cm4.h>
#include <cmsis_compiler.h>

#include <tusb.h>
#include <tinyusb/src/class/midi/midi_device.h>
#include <tinyusb/src/class/midi/midi.h>
#include <tinyusb/src/class/cdc/cdc_device.h>

#include "protocol/parser.hpp"
#include "protocol/codec.hpp"
#include "protocol/types.hpp"
#include "protocol/constants.hpp"

#include <gfx/text.hpp>
#include <gfx/icon.hpp>
//#include "stmcpp/register.hpp"
//#include "stmcpp/units.hpp"
//#include "stmcpp/systick.hpp"

using namespace stmcpp::units;

// Transportní rozhraní, ze kterého paket přišel
enum class Transport : uint8_t {
	USB_CDC,
	BLE,
};

// USB CDC protokolový parser
static protocol::Parser _cdcParser;
static uint8_t _cdcPacketId = 0;

// Keepalive: čas posledního paketu z PC
static duration _lastPcKeepalive = 0_ms;

bool isPcAlive() {
	return (stmcpp::systick::getDuration() - _lastPcKeepalive) < 5000_ms;
}

// Odeslání paketu přes USB CDC
static void sendCdcPacket(const protocol::Packet &pkt) {
	uint8_t buf[protocol::MAX_FRAME_SIZE];
	uint16_t len = protocol::encode(pkt, buf);
	if (len > 0) {
		tud_cdc_write(buf, len);
		tud_cdc_write_flush();
	}
}

// Odeslání ACK paketu
static void sendAck(uint8_t dest, uint8_t ackedPacketId, Transport via) {
	protocol::Packet ack = {};
	ack.source = protocol::ADDR_BASE;
	ack.dest = dest;
	ack.packetId = _cdcPacketId++;
	ack.msgType = protocol::MsgType::ACK;
	ack.payload[0] = ackedPacketId;
	ack.payloadLen = 1;

	if (via == Transport::USB_CDC) {
		sendCdcPacket(ack);
	} else {
		Bluetooth::sendProtocolPacket(ack);
	}
}

// Odeslání NAK paketu
static void sendNak(uint8_t dest, uint8_t ackedPacketId, protocol::NakReason reason, Transport via) {
	protocol::Packet nak = {};
	nak.source = protocol::ADDR_BASE;
	nak.dest = dest;
	nak.packetId = _cdcPacketId++;
	nak.msgType = protocol::MsgType::NAK;
	nak.payload[0] = ackedPacketId;
	nak.payload[1] = static_cast<uint8_t>(reason);
	nak.payloadLen = 2;

	if (via == Transport::USB_CDC) {
		sendCdcPacket(nak);
	} else {
		Bluetooth::sendProtocolPacket(nak);
	}
}

// Zpracování lokálního paketu (určeného pro Base)
static void handleLocalPacket(const protocol::Packet &pkt, Transport via) {
	bool needsAck = (pkt.msgType != protocol::MsgType::ACK && pkt.msgType != protocol::MsgType::NAK
					 && pkt.msgType != protocol::MsgType::KEEPALIVE && pkt.msgType != protocol::MsgType::KEEPALIVE_ACK
					 && pkt.msgType != protocol::MsgType::NOW_PLAYING_INFO);

	switch (pkt.msgType) {
		case protocol::MsgType::SET_CURRENT:
			if (pkt.payloadLen >= 1) {
				base::current::set(static_cast<bool>(pkt.payload[0]));
				if (needsAck) sendAck(pkt.source, pkt.packetId, via);
			} else {
				sendNak(pkt.source, pkt.packetId, protocol::NakReason::INVALID_PAYLOAD, via);
			}
			break;

		case protocol::MsgType::GET_STATUS: {
			protocol::Packet resp = {};
			resp.source = protocol::ADDR_BASE;
			resp.dest = pkt.source;
			resp.packetId = _cdcPacketId++;
			resp.msgType = protocol::MsgType::STATUS_RESP;
			resp.payload[0] = static_cast<uint8_t>(Bluetooth::isConnected());
			resp.payload[1] = static_cast<uint8_t>(base::current::isEnabled());
			resp.payload[2] = static_cast<uint8_t>(display::isConnected());
			resp.payload[3] = static_cast<uint8_t>(isPcAlive());
			resp.payloadLen = 4;

			if (via == Transport::USB_CDC) {
				sendCdcPacket(resp);
			} else {
				Bluetooth::sendProtocolPacket(resp);
			}
			if (needsAck) sendAck(pkt.source, pkt.packetId, via);
			break;
		}

		case protocol::MsgType::PLAY_SONG: {
			// Zobrazení "Now Playing" na OLED
			if (pkt.payloadLen > 0) {
				char songName[64] = {};
				uint8_t len = pkt.payloadLen < sizeof(songName) - 1 ? pkt.payloadLen : sizeof(songName) - 1;
				memcpy(songName, pkt.payload, len);
				songName[len] = '\0';
				GUI::showNowPlaying(songName);
			}
			if (needsAck) sendAck(pkt.source, pkt.packetId, via);
			break;
		}

		case protocol::MsgType::STOP_SONG:
			GUI::dismissNowPlaying();
			if (needsAck) sendAck(pkt.source, pkt.packetId, via);
			break;

		case protocol::MsgType::NOW_PLAYING_INFO: {
			// Payload: [song_h, song_l, verse, letter, led] — 0xFF = beze změny
			if (pkt.payloadLen >= 5) {
				uint8_t song_h = pkt.payload[0];
				uint8_t song_l = pkt.payload[1];
				uint8_t verse_bcd = pkt.payload[2];
				uint8_t letter_raw = pkt.payload[3];
				uint8_t led_raw = pkt.payload[4];

				// Song: 2 BCD bajty → uint16_t
				if (song_h != 0xFF && song_l != 0xFF) {
					uint16_t d3 = (song_h >> 4) & 0x0F;
					uint16_t d2 = song_h & 0x0F;
					uint16_t d1 = (song_l >> 4) & 0x0F;
					uint16_t d0 = song_l & 0x0F;
					uint16_t song = d3 * 1000 + d2 * 100 + d1 * 10 + d0;
					display::setSong(song, true);
				}

				// Verse: BCD
				if (verse_bcd != 0xFF) {
					uint8_t v1 = (verse_bcd >> 4) & 0x0F;
					uint8_t v0 = verse_bcd & 0x0F;
					uint8_t verse = v1 * 10 + v0;
					display::setVerse(verse, true);
				}

				// Letter: 0xA=A .. 0xD=D, 0xF=off
				if (letter_raw != 0xFF) {
					if (letter_raw >= 0x0A && letter_raw <= 0x0D) {
						display::setLetter('A' + (letter_raw - 0x0A), true);
					} else {
						display::setLetter(' ', false);
					}
				}

				// LED: 1=red, 2=green, 3=blue, 4=yellow, 0xF=off
				if (led_raw != 0xFF) {
					switch (led_raw) {
						case 1: display::setLed(display::ledColor::RED); break;
						case 2: display::setLed(display::ledColor::GREEN); break;
						case 3: display::setLed(display::ledColor::BLUE); break;
						case 4: display::setLed(display::ledColor::YELLOW); break;
						default: display::setLed(display::ledColor::OFF); break;
					}
				}

				GUI::updateNowPlayingInfo();
			}
			// Bez ACK — časté aktualizace
			break;
		}

		case protocol::MsgType::SIGNAL_PLAYING:
		case protocol::MsgType::SIGNAL_RECORDING:
		case protocol::MsgType::SIGNAL_STOPPED:
		case protocol::MsgType::RECORD_SONG:
		case protocol::MsgType::GET_SONG_LIST:
		case protocol::MsgType::SONG_LIST_RESP:
		case protocol::MsgType::STATUS_RESP:
			if (needsAck) sendAck(pkt.source, pkt.packetId, via);
			break;

		case protocol::MsgType::KEEPALIVE: {
			// Odpovědět KEEPALIVE_ACK odesílateli
			protocol::Packet ackPkt = {};
			ackPkt.source = protocol::ADDR_BASE;
			ackPkt.dest = pkt.source;
			ackPkt.packetId = _cdcPacketId++;
			ackPkt.msgType = protocol::MsgType::KEEPALIVE_ACK;
			ackPkt.payloadLen = 0;

			if (via == Transport::USB_CDC) {
				sendCdcPacket(ackPkt);
			} else {
				Bluetooth::sendProtocolPacket(ackPkt);
			}
			break;
		}

		case protocol::MsgType::KEEPALIVE_ACK:
			// Ignorujeme — Base nepotřebuje sledovat ACK od PC
			break;

		case protocol::MsgType::ACK:
		case protocol::MsgType::NAK:
			// Potvrzení — prozatím jen ignorujeme
			break;

		default:
			sendNak(pkt.source, pkt.packetId, protocol::NakReason::UNKNOWN_MSG, via);
			break;
	}
}

// Směrování paketu podle cílové adresy
static void processProtocolPacket(const protocol::Packet &pkt, Transport via) {
	// Aktualizace keepalive timestampu při každém paketu z PC
	if (via == Transport::USB_CDC) {
		_lastPcKeepalive = stmcpp::systick::getDuration();
	}

	bool forBase = (pkt.dest == protocol::ADDR_BASE || pkt.dest == protocol::ADDR_BROADCAST);

	// Lokální zpracování
	if (forBase) {
		handleLocalPacket(pkt, via);
	}

	// Přeposlání na Remote přes BLE (pokud nepřišel z BLE)
	if ((pkt.dest == protocol::ADDR_REMOTE || pkt.dest == protocol::ADDR_BROADCAST) && via != Transport::BLE) {
		Bluetooth::sendProtocolPacket(pkt);
	}

	// Přeposlání na PC přes CDC (pokud nepřišel z CDC)
	if ((pkt.dest == protocol::ADDR_PC || pkt.dest == protocol::ADDR_BROADCAST) && via != Transport::USB_CDC) {
		sendCdcPacket(pkt);
	}
}

extern stmcpp::scheduler::Scheduler oledSleepScheduler;
extern stmcpp::scheduler::Scheduler keypressScheduler;
extern stmcpp::scheduler::Scheduler guiRenderScheduler;
extern stmcpp::scheduler::Scheduler commTimeoutScheduler;
extern stmcpp::scheduler::Scheduler dispChangeScheduler;
extern void signalError();

extern Oled::OledBuffer frameBuffer;

stmcpp::scheduler::Scheduler ledScheduler(500_ms, [](){
	display::sendState();
}, true, false);

// Callback: uživatel stiskl encoder na "Now Playing" → odešleme STOP_SONG
static void onUserStopPlayback() {
	protocol::Packet stopPkt = {};
	stopPkt.source = protocol::ADDR_BASE;
	stopPkt.dest = protocol::ADDR_PC;
	stopPkt.packetId = _cdcPacketId++;
	stopPkt.msgType = protocol::MsgType::STOP_SONG;
	stopPkt.payloadLen = 0;
	sendCdcPacket(stopPkt);

	// Také na Remote přes BLE
	stopPkt.dest = protocol::ADDR_REMOTE;
	stopPkt.packetId = _cdcPacketId++;
	Bluetooth::sendProtocolPacket(stopPkt);
}

// One-shot scheduler: auto-dismiss the splash screen after 3 seconds
stmcpp::scheduler::Scheduler splashDismissScheduler(3000_ms, [](){
	GUI::keypress(ui::InputEvent::Enter);
}, false, false);

// One-shot scheduler: show pairing screen if SECURED doesn't arrive in time
stmcpp::scheduler::Scheduler pairingDelayScheduler(500_ms, [](){
	if (Bluetooth::isConnected() && !Bluetooth::isBonded()) {
		GUI::showPairingConfirm();
		GUI::renderForce();
	}
}, false, false);

extern "C" void SystemInit(void) {

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

	Debug::setLevel(Debug::Level::INFO);

	//Initialize the OLED
	Oled::init();
	GUI::init();
	GUI::setStopCallback(onUserStopPlayback);
	splashDismissScheduler.start();
	//Check if we want to enable DFU
	//base::dfuCheck();
	//Start the watchdog
	//base::wdtStart();
	//Initialize MIDIlow
	midi::init();

	LED::init();
	//Initialize LED indicator

	//LED::frontStrip.setColor(LED::Color(0, 0, 255, 0.1)); // Set front strip to blue
	ledScheduler.resume(); // Start the LED scheduler

	//Initialize bluetooth
	if(Bluetooth::init()){
		signalError();
	}
	
	//Initialize LED display
	display::init();

	NVIC_EnableIRQ(USB_LP_IRQn);
	NVIC_EnableIRQ(USB_HP_IRQn);
	
	CRS->CFGR &= ~CRS_CFGR_SYNCSRC;
	CRS->CFGR |= (0b10 << CRS_CFGR_SYNCSRC_Pos);

	CRS->CFGR |= CRS_CR_AUTOTRIMEN;
	CRS->CFGR |= CRS_CR_CEN;

	usb::init();

	if(Bluetooth::configure()){
		signalError();
	}



	//RCC->CCIPR &= ~(RCC_CCIPR_CLK48SEL_Msk << RCC_CCIPR_CLK48SEL_Pos);
	//RCC->CCIPR |= (clksel << RCC_CCIPR_CLK48SEL_SHIFT);




	//oledSleepScheduler.pause();


	uint8_t packet[4];

	while (1) {

		tud_task();

		if(tud_midi_available()) {
			// Read MIDI messages
			tud_midi_packet_read(&packet[0]);
			midi::send(packet);
		}

		// Čtení protokolových dat z USB CDC
		if (tud_cdc_available()) {
			uint8_t cdcBuf[64];
			uint32_t count = tud_cdc_read(cdcBuf, sizeof(cdcBuf));
			for (uint32_t i = 0; i < count; ++i) {
				if (_cdcParser.feed(cdcBuf[i])) {
					processProtocolPacket(_cdcParser.getPacket(), Transport::USB_CDC);
				}
			}
		}

		// Kontrola protokolových paketů z BLE
		if (Bluetooth::isProtocolPacketAvailable()) {
			processProtocolPacket(Bluetooth::getProtocolPacket(), Transport::BLE);
		}

		keypressScheduler.dispatch();
		guiRenderScheduler.dispatch();
		splashDismissScheduler.dispatch();
		pairingDelayScheduler.dispatch();
		ledScheduler.dispatch();

		if(Bluetooth::isCommandAvailable()){
			std::unique_ptr<Bluetooth::Command> command = Bluetooth::getCommand();
			Bluetooth::Command::Type type = command->getType();
			
			// --- WARNING: Increased buffer size ---
			// 64 bytes is too small for commands with addresses, UUIDs, or hex strings.
			// 256 bytes is a safer choice.
			char packet[64]; 

			// Use a switch for better readability
			// Use snprintf(packet, sizeof(packet), ...) for buffer safety
			switch (type) {
				// --- Simple Commands ---
				case Bluetooth::Command::Type::ADV_TIMEOUT:     snprintf(packet, sizeof(packet), "Got ble: ADV_TIMEOUT\r\n"); break;
				case Bluetooth::Command::Type::BONDED:
					Bluetooth::setBonded(true);
					pairingDelayScheduler.stop();
					snprintf(packet, sizeof(packet), "Got ble: BONDED\r\n"); break;
				case Bluetooth::Command::Type::DISCONNECT:
					Bluetooth::setConnected(false);
					pairingDelayScheduler.stop();
					GUI::renderForce();
					snprintf(packet, sizeof(packet), "Got ble: DISCONNECT\r\n"); break;
				case Bluetooth::Command::Type::ERR_CONNPARAM:   snprintf(packet, sizeof(packet), "Got ble: ERR_CONNPARAM\r\n"); break;
				case Bluetooth::Command::Type::ERR_MEMORY:      snprintf(packet, sizeof(packet), "Got ble: ERR_MEMORY\r\n"); break;
				case Bluetooth::Command::Type::ERR_READ:        snprintf(packet, sizeof(packet), "Got ble: ERR_READ\r\n"); break;
				case Bluetooth::Command::Type::ERR_RMT_CMD:     snprintf(packet, sizeof(packet), "Got ble: ERR_RMT_CMD\r\n"); break;
				case Bluetooth::Command::Type::ERR_SEC:         snprintf(packet, sizeof(packet), "Got ble: ERR_SEC\r\n"); break;
				case Bluetooth::Command::Type::KEY_REQ:         snprintf(packet, sizeof(packet), "Got ble: KEY_REQ\r\n"); break;
				case Bluetooth::Command::Type::PIO1H:           snprintf(packet, sizeof(packet), "Got ble: PIO1H\r\n"); break;
				case Bluetooth::Command::Type::PIO1L:           snprintf(packet, sizeof(packet), "Got ble: PIO1L\r\n"); break;
				case Bluetooth::Command::Type::PIO2H:           snprintf(packet, sizeof(packet), "Got ble: PIO2H\r\n"); break;
				case Bluetooth::Command::Type::PIO2L:           snprintf(packet, sizeof(packet), "Got ble: PIO2L\r\n"); break;
				case Bluetooth::Command::Type::PIO3H:           snprintf(packet, sizeof(packet), "Got ble: PIO3H\r\n"); break;
				case Bluetooth::Command::Type::PIO3L:           snprintf(packet, sizeof(packet), "Got ble: PIO3L\r\n"); break;
				case Bluetooth::Command::Type::RE_DISCV:        snprintf(packet, sizeof(packet), "Got ble: RE_DISCV\r\n"); break;
				case Bluetooth::Command::Type::REBOOT:          snprintf(packet, sizeof(packet), "Got ble: REBOOT\r\n"); break;
				case Bluetooth::Command::Type::RMT_CMD_OFF:     snprintf(packet, sizeof(packet), "Got ble: RMT_CMD_OFF\r\n"); break;
				case Bluetooth::Command::Type::RMT_CMD_ON:      snprintf(packet, sizeof(packet), "Got ble: RMT_CMD_ON\r\n"); break;
				case Bluetooth::Command::Type::SECURED:
					Bluetooth::setBonded(true);
					pairingDelayScheduler.stop();
					snprintf(packet, sizeof(packet), "Got ble: SECURED\r\n"); break;
				case Bluetooth::Command::Type::STREAM_OPEN:     snprintf(packet, sizeof(packet), "Got ble: STREAM_OPEN\r\n"); break;
				case Bluetooth::Command::Type::TMR1:            snprintf(packet, sizeof(packet), "Got ble: TMR1\r\n"); break;
				case Bluetooth::Command::Type::TMR2:            snprintf(packet, sizeof(packet), "Got ble: TMR2\r\n"); break;
				case Bluetooth::Command::Type::TMR3:            snprintf(packet, sizeof(packet), "Got ble: TMR3\r\n"); break;

				// --- Parameterized Commands ---
				case Bluetooth::Command::Type::CONN_PARAM: {
					auto* cmd = static_cast<Bluetooth::ConnParamCommand*>(command.get());
					snprintf(packet, sizeof(packet), "Got ble: CONN_PARAM int=%d, lat=%d, to=%d\r\n", 
							cmd->getInterval(), cmd->getLatency(), cmd->getTimeout());
					break;
				}
				case Bluetooth::Command::Type::CONNECT: {
					auto* cmd = static_cast<Bluetooth::ConnectCommand*>(command.get());
					if (cmd->getConnected() == 1) {
						Bluetooth::setConnected(true);
						Bluetooth::setBonded(false);
						// Delay pairing screen — if SECURED arrives first, device is already bonded
						pairingDelayScheduler.start();
					} else {
						Bluetooth::setConnected(false);
						pairingDelayScheduler.stop();
					}
					GUI::renderForce();
					snprintf(packet, sizeof(packet), "Got ble: CONNECT conn=%d, addr=%s\r\n",
							cmd->getConnected(), cmd->getAddr().c_str());
					break;
				}
				case Bluetooth::Command::Type::KEY: {
					auto* cmd = static_cast<Bluetooth::KeyCommand*>(command.get());
					snprintf(packet, sizeof(packet), "Got ble: KEY key=%d\r\n", cmd->getKey());
					break;
				}
				case Bluetooth::Command::Type::INDI: {
					auto* cmd = static_cast<Bluetooth::IndiCommand*>(command.get());
					snprintf(packet, sizeof(packet), "Got ble: INDI hdl=%d, hex=%s\r\n", 
							cmd->getHandle(), cmd->getHex().c_str());
					break;
				}
				case Bluetooth::Command::Type::NOTI: {
					auto* cmd = static_cast<Bluetooth::NotiCommand*>(command.get());
					snprintf(packet, sizeof(packet), "Got ble: NOTI hdl=%d, hex=%s\r\n", 
							cmd->getHandle(), cmd->getHex().c_str());
					break;
				}
				case Bluetooth::Command::Type::RV: {
					auto* cmd = static_cast<Bluetooth::RvCommand*>(command.get());
					snprintf(packet, sizeof(packet), "Got ble: RV hdl=%d, hex=%s\r\n", 
							cmd->getHandle(), cmd->getHex().c_str());
					break;
				}
				case Bluetooth::Command::Type::S_RUN: {
					auto* cmd = static_cast<Bluetooth::SRunCommand*>(command.get());
					snprintf(packet, sizeof(packet), "Got ble: S_RUN cmd=%s\r\n", cmd->getCmd().c_str());
					break;
				}
				case Bluetooth::Command::Type::WC: {
					auto* cmd = static_cast<Bluetooth::WcCommand*>(command.get());
					snprintf(packet, sizeof(packet), "Got ble: WC hdl=%d, hex=%s\r\n", 
							cmd->getHandle(), cmd->getHex().c_str());
					break;
				}
				case Bluetooth::Command::Type::WV: {
					auto* cmd = static_cast<Bluetooth::WvCommand*>(command.get());
					snprintf(packet, sizeof(packet), "Got ble: WV hdl=%d, hex=%s\r\n", 
							cmd->getHandle(), cmd->getHex().c_str());
					break;
				}
				case Bluetooth::Command::Type::ADV_CONNECTABLE: {
					auto* cmd = static_cast<Bluetooth::AdvConnectableCommand*>(command.get());
					snprintf(packet, sizeof(packet), "Got ble: ADV_CONN addr=%s, conn=%d, name=%s, uuids=%s, rssi=%d\r\n",
							cmd->getAddr().c_str(), cmd->getConnected(), cmd->getName().c_str(), 
							cmd->getUuids().c_str(), cmd->getRssi());
					break;
				}
				case Bluetooth::Command::Type::ADV_NON_CONNECTABLE: {
					auto* cmd = static_cast<Bluetooth::AdvNonConnectableCommand*>(command.get());
					snprintf(packet, sizeof(packet), "Got ble: ADV_NON_CONN addr=%s, conn=%d, rssi=%d, hex=%s\r\n",
							cmd->getAddr().c_str(), cmd->getConnected(), cmd->getRssi(), cmd->getHex().c_str());
					break;
				}
				
				// --- Fallback ---
				case Bluetooth::Command::Type::UNKNOWN:
				default:
					snprintf(packet, sizeof(packet), "Got ble: UNKNOWN (%02X)\r\n", static_cast<uint8_t>(type));
					break;
			}

			tud_cdc_write_str(packet);
		}



		//LED::update();
		// Reset the Independent Watchdog Timer (IWDG)
		//IWDG->KR = 0xAAAA;

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
