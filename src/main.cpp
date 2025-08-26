#include "main.hpp"
#include "oled.hpp"
#include "base.hpp"
#include "scheduler.hpp"
#include "menu.hpp"
#include "midi.hpp"
#include "led.hpp"
#include "ble.hpp"
#include "display.hpp"
#include "usb.hpp"

#include <utility>

#include <stm32g431xx.h>
#include <core_cm4.h>
#include <cmsis_compiler.h>

#include <tusb.h>
#include <tinyusb/src/class/midi/midi_device.h>
#include <tinyusb/src/class/midi/midi.h>
#include "stmcpp/register.hpp"

extern Scheduler oledSleepScheduler;
extern Scheduler keypressScheduler;
extern Scheduler keepaliveScheduler;
extern Scheduler guiRenderScheduler;
extern Scheduler menuScrollScheduler;
extern Scheduler commTimeoutScheduler;
extern Scheduler dispChangeScheduler;

Scheduler ledScheduler(500, [](){ 
	display::sendState();

}, Scheduler::DISPATCH_ON_INCREMENT | Scheduler::PERIODICAL);

Scheduler startupSplashScheduler(2000, [](void){GUI::displayActiveMenu(); oledSleepScheduler.resume(); startupSplashScheduler.pause();}, Scheduler::ACTIVE | Scheduler::DISPATCH_ON_INCREMENT);


extern "C" void SystemInit(void) {

	// Enable the FPU if needed
	#if (__FPU_PRESENT == 1) && (__FPU_USED == 1)
		stmcpp::reg::set(std::ref(SCB->CPACR), (3UL << 20U) | (3UL << 22U));
    #endif


}

extern "C" void SysTick_Handler(void){
	//oledSleepScheduler.increment();
	keypressScheduler.increment();
	guiRenderScheduler.increment();
	menuScrollScheduler.increment();
	startupSplashScheduler.increment();
	//commTimeoutScheduler.increment();
	//dispChangeScheduler.increment();
    ledScheduler.increment();
}


extern "C" int main(void)
{
	//Initialize io and other stuff related to the base unitusart_tx
	Base::init();

	//Initialize the OLED
	Oled::init();
	//Check if we want to enable DFU
	//Base::dfuCheck();
	//Start the watchdog
	//Base::wdtStart();
	//Initialize MIDIlow
	midi::init();

	LED::init();
	//Initialize LED indicator

	//LED::frontStrip.setColor(LED::Color(0, 0, 255, 0.1)); // Set front strip to blue
	ledScheduler.resume(); // Start the LED scheduler

	//Initialize bluetooth
	//BLE::init();
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



		//LED::update();
		// Reset the Independent Watchdog Timer (IWDG)
		//IWDG->KR = 0xAAAA;

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
