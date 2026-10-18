#include "usb.hpp"
#include <tusb.h>
#include <tinyusb/src/class/midi/midi_device.h>
#include <tinyusb/src/class/midi/midi.h>
#include "led.hpp"
#include "stmcpp/register.hpp"
#include "stmcpp/gpio.hpp"
#include <stm32g431xx.h>

namespace usb {
    void init() {
        stmcpp::gpio::pin<stmcpp::gpio::port::porta, 11> usb_dm (stmcpp::gpio::mode::input, stmcpp::gpio::otype::pushPull, stmcpp::gpio::speed::veryHigh, stmcpp::gpio::pull::noPull);
		stmcpp::gpio::pin<stmcpp::gpio::port::porta, 12> usb_dp (stmcpp::gpio::mode::input, stmcpp::gpio::otype::pushPull, stmcpp::gpio::speed::veryHigh, stmcpp::gpio::pull::noPull);

        // TX FIFO CDC nesmí být přepisovatelná ani bez DTR — přepsáním by se roztrhly protokolové rámce
        tud_cdc_configure_t cdcConfig = TUD_CDC_CONFIGURE_DEFAULT();
        cdcConfig.tx_overwritabe_if_not_connected = false;
        tud_cdc_configure(&cdcConfig);

        tusb_init();

        // Deska má pevný 1k5 pull-up na D+. dcd_init() v TinyUSB (stm32_fsdev) navíc zapne interní
        // pull-up (USB_BCDR.DPPU); dcd_connect() je tam silný symbol, takže ho jen hned vypneme.
        // Nikde jinde se dcd_connect()/tud_connect() nevolá.
        USB->BCDR &= ~USB_BCDR_DPPU;
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
