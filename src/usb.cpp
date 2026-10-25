#include "usb.hpp"
#include <tusb.h>
#include <tinyusb/src/class/midi/midi_device.h>
#include <tinyusb/src/class/midi/midi.h>
#include "led.hpp"
#include "debug.hpp"
#include "stmcpp/register.hpp"
#include "stmcpp/gpio.hpp"
#include <stm32g431xx.h>

namespace usb {
    void init() {
        stmcpp::gpio::pin<stmcpp::gpio::port::porta, 11> usb_dm (stmcpp::gpio::mode::input, stmcpp::gpio::otype::pushPull, stmcpp::gpio::speed::veryHigh, stmcpp::gpio::pull::noPull);
		stmcpp::gpio::pin<stmcpp::gpio::port::porta, 12> usb_dp (stmcpp::gpio::mode::input, stmcpp::gpio::otype::pushPull, stmcpp::gpio::speed::veryHigh, stmcpp::gpio::pull::noPull);

        // The CDC TX FIFO must not be overwritable even without DTR — overwriting would tear protocol frames apart.
        // The configuration applies to both instances (CDC 0 protocol, CDC 1 log); the log writes only whole lines into the FIFO and only with DTR.
        tud_cdc_configure_t cdcConfig = TUD_CDC_CONFIGURE_DEFAULT();
        cdcConfig.tx_overwritabe_if_not_connected = false;
        tud_cdc_configure(&cdcConfig);

        tusb_init();

        // The internal D+ pull-up (USB_BCDR.DPPU, enabled in dcd_init()) stays enabled: the fixed 1k5 (R33)
        // does not work on the boards in use, without the internal pull-up the device does not announce itself to the host.
    }

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


// The TinyUSB callbacks are called by tud_task() from the main loop → Debug::log is allowed here
void tud_mount_cb(void) {
	Debug::log(Debug::Level::INFO, "usb", "mounted");
}

// Invoked when device is unmounted
void tud_umount_cb(void) {
	Debug::log(Debug::Level::INFO, "usb", "unmounted");
}

// Invoked when usb bus is suspended
// remote_wakeup_en : if host allow us  to perform remote wakeup
// Within 7ms, device must draw an average of current less than 2.5 mA from bus
void tud_suspend_cb(bool remote_wakeup_en) {
    (void) remote_wakeup_en;
	Debug::log(Debug::Level::INFO, "usb", "suspend");

}

// Invoked when usb bus is resumed
void tud_resume_cb(void) {
	Debug::log(Debug::Level::INFO, "usb", "resume");
}

// Port opened/closed on the PC (DTR); CDC 0 = protocol, CDC 1 = log
void tud_cdc_line_state_cb(uint8_t itf, bool dtr, bool rts) {
	(void) rts;
	Debug::log(Debug::Level::INFO, "usb", "%s port %s", itf == 0 ? "protocol" : "log", dtr ? "opened" : "closed");
}
