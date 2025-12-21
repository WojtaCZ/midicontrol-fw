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
//#include "stmcpp/register.hpp"
//#include "stmcpp/units.hpp"
//#include "stmcpp/systick.hpp"

using namespace stmcpp::units;

extern stmcpp::scheduler::Scheduler oledSleepScheduler;
extern stmcpp::scheduler::Scheduler keypressScheduler;
extern stmcpp::scheduler::Scheduler keepaliveScheduler;
extern stmcpp::scheduler::Scheduler guiRenderScheduler;
extern stmcpp::scheduler::Scheduler menuScrollScheduler;
extern stmcpp::scheduler::Scheduler commTimeoutScheduler;
extern stmcpp::scheduler::Scheduler dispChangeScheduler;
extern void signalError();

stmcpp::scheduler::Scheduler ledScheduler(500_ms, [](){ 
	display::sendState();
}, true, false);

stmcpp::scheduler::Scheduler startupSplashScheduler(2000_ms, [](void){GUI::displayActiveMenu(); oledSleepScheduler.resume(); startupSplashScheduler.pause();}, false, true);


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


extern "C" int main(void)
{
	stmcpp::systick::enable(144_MHz, 1_ms);

	Debug::setLevel(Debug::Level::INFO);

	//Initialize the OLED
	Oled::init();
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




	//RCC->CCIPR &= ~(RCC_CCIPR_CLK48SEL_Msk << RCC_CCIPR_CLK48SEL_Pos);
	//RCC->CCIPR |= (clksel << RCC_CCIPR_CLK48SEL_SHIFT);

	


	oledSleepScheduler.pause();


	uint8_t packet[4];

	while (1) {

		tud_task();	

		if(tud_midi_available()) {
			// Read MIDI messages
			tud_midi_packet_read(&packet[0]);
			midi::send(packet);
		}

		//oledSleepScheduler.increment();
		keypressScheduler.dispatch();
		guiRenderScheduler.dispatch();
		menuScrollScheduler.dispatch();
		startupSplashScheduler.dispatch();
		//commTimeoutScheduler.increment();
		//dispChangeScheduler.increment();
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
				case Bluetooth::Command::Type::BONDED:          snprintf(packet, sizeof(packet), "Got ble: BONDED\r\n"); break;
				case Bluetooth::Command::Type::DISCONNECT:      snprintf(packet, sizeof(packet), "Got ble: DISCONNECT\r\n"); break;
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
				case Bluetooth::Command::Type::SECURED:         snprintf(packet, sizeof(packet), "Got ble: SECURED\r\n"); break;
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
