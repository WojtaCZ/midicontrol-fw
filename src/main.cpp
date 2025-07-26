#include "main.hpp"
#include "oled.hpp"
#include "base.hpp"
#include "scheduler.hpp"
#include "menu.hpp"
#include "midi.hpp"
#include "led.hpp"
#include "ble.hpp"
#include "display.hpp"

#include <utility>

#include <stm32g431xx.h>
#include <core_cm4.h>
#include <cmsis_compiler.h>

#include <tusb.h>

extern Scheduler oledSleepScheduler;
extern Scheduler keypressScheduler;
extern Scheduler keepaliveScheduler;
extern Scheduler guiRenderScheduler;
extern Scheduler menuScrollScheduler;
extern Scheduler commTimeoutScheduler;
extern Scheduler dispChangeScheduler;

Scheduler startupSplashScheduler(2000, [](void){GUI::displayActiveMenu(); oledSleepScheduler.resume(); startupSplashScheduler.pause();}, Scheduler::ACTIVE | Scheduler::DISPATCH_ON_INCREMENT);


extern "C" void SystemInit(void) {

	//Initialize io and other stuff related to the base unit
	Base::init();
	//Initialize the OLED
	Oled::init();
	//Check if we want to enable DFU
	Base::dfuCheck();
	//Start the watchdog
	//Base::wdtStart();
	//Initialize MIDI
	MIDI::init();
	//Initialize LED indicators
	LED::init();
	//Initialize bluetooth
	BLE::init();
	//Initialize LED display
	Display::init();

	NVIC_EnableIRQ(USB_LP_IRQn);
	NVIC_EnableIRQ(USB_HP_IRQn);
	
	CRS->CFGR &= ~CRS_CFGR_SYNCSRC;
	CRS->CFGR |= (0b10 << CRS_CFGR_SYNCSRC_Pos);

	CRS->CFGR |= CRS_CR_AUTOTRIMEN;
	CRS->CFGR |= CRS_CR_CEN;

	//RCC->CCIPR &= ~(RCC_CCIPR_CLK48SEL_Msk << RCC_CCIPR_CLK48SEL_Pos);
	//RCC->CCIPR |= (clksel << RCC_CCIPR_CLK48SEL_SHIFT);

	tusb_init();

	oledSleepScheduler.pause();

}

extern "C" void SysTick_Handler(void){
	oledSleepScheduler.increment();
	keypressScheduler.increment();
	guiRenderScheduler.increment();
	menuScrollScheduler.increment();
	startupSplashScheduler.increment();
	commTimeoutScheduler.increment();
	dispChangeScheduler.increment();
}


extern "C" int main(void)
{
	while (1) {

		tud_task();	
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

extern "C" void USB_HP_IRQHandler(void) {
	tud_int_handler(0);
}

extern "C" void USB_LP_IRQHandler(void) {
	tud_int_handler(0);
}

extern "C" void USBWakeUp_IRQHandler(void) {
	tud_int_handler(0);
}

// USB PD
extern "C" void UCPD1_IRQHandler(void) {
	tuc_int_handler(0);
}


  void tud_mount_cb(void) {
  }
  
  // Invoked when device is unmounted
  void tud_umount_cb(void) {
  }
  
  // Invoked when usb bus is suspended
  // remote_wakeup_en : if host allow us  to perform remote wakeup
  // Within 7ms, device must draw an average of current less than 2.5 mA from bus
  void tud_suspend_cb(bool remote_wakeup_en) {
	(void) remote_wakeup_en;
  }
  
  // Invoked when usb bus is resumed
  void tud_resume_cb(void) {
  }
  
  
  // Invoked when cdc when line state changed e.g connected/disconnected
  void tud_cdc_line_state_cb(uint8_t itf, bool dtr, bool rts) {
	(void) itf;
	(void) rts;
  
	// TODO set some indicator
	if (dtr) {
	  // Terminal connected
	} else {
	  // Terminal disconnected
	}
  }
  
  // Invoked when CDC interface received data from host
  void tud_cdc_rx_cb(uint8_t itf) {
	(void) itf;
  }
  