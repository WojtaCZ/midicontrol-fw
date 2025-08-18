#include "usb.hpp"
#include <tusb.h>
#include <tinyusb/src/class/midi/midi_device.h>
#include <tinyusb/src/class/midi/midi.h>
#include "led.hpp"
#include "stmcpp/register.hpp"
#include "stmcpp/gpio.hpp"

namespace usb {
    void init() {
        stmcpp::gpio::pin<stmcpp::gpio::port::porta, 11> usb_dm (stmcpp::gpio::mode::input, stmcpp::gpio::otype::pushPull, stmcpp::gpio::speed::veryHigh, stmcpp::gpio::pull::noPull);
		stmcpp::gpio::pin<stmcpp::gpio::port::porta, 12> usb_dp (stmcpp::gpio::mode::input, stmcpp::gpio::otype::pushPull, stmcpp::gpio::speed::veryHigh, stmcpp::gpio::pull::noPull);
        tusb_init();
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


void tud_mount_cb(void) {
    LED::usb.setColor(LED::Color(0, 255, 0, 1));
}

// Invoked when device is unmounted
void tud_umount_cb(void) {
    LED::usb.setColor(LED::Color(0, 0, 0, 0));
}

// Invoked when usb bus is suspended
// remote_wakeup_en : if host allow us  to perform remote wakeup
// Within 7ms, device must draw an average of current less than 2.5 mA from bus
void tud_suspend_cb(bool remote_wakeup_en) {
    (void) remote_wakeup_en;
    LED::usb.setColor(LED::Color(255, 255, 0, 0.5));
}

// Invoked when usb bus is resumed
void tud_resume_cb(void) {
    LED::usb.setColor(LED::Color(0, 255, 0, 1));
}

