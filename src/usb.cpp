#include "usb.hpp"
#include <tusb.h>
#include <tinyusb/src/class/midi/midi_device.h>
#include <tinyusb/src/class/midi/midi.h>
#include "led.hpp"
#include "stmcpp/register.hpp"
#include "stmcpp/gpio.hpp"
#include "comm.hpp"

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

char messageBuffer[512];
uint32_t messageLength;

bool tud_vendor_control_xfer_cb(uint8_t rhport, uint8_t stage, tusb_control_request_t const* request) {
  switch (request->bmRequestType_bit.type) {
    case TUSB_REQ_TYPE_VENDOR:

      if(request->bmRequestType_bit.direction == TUSB_DIR_IN){
        // If data is requested from the device
        switch (stage)
        {
          // If the setup is completed, we should provide some data back
          case CONTROL_STAGE_SETUP:
            communication::processBinaryCommand(static_cast<communication::command>(request->bRequest), communication::direction::GET, request->wIndex, request->wValue, messageBuffer, &messageLength);
            return tud_control_xfer(rhport, request, (void*) (uintptr_t) messageBuffer, messageLength);
            break;
          
          // Nothing to do here
          case CONTROL_STAGE_DATA:
          case CONTROL_STAGE_ACK:
            return true;

          default:
            break;
        }
      } else if (request->bmRequestType_bit.direction == TUSB_DIR_OUT){
        // If data was sent to the device
        switch (stage)
        {
          // If the setup is completed, deal with what we've received
          case CONTROL_STAGE_SETUP:
            messageLength = request->wLength;
            return tud_control_xfer(rhport, request, &messageBuffer, request->wLength);
            break;

          // Nothing to do here
          case CONTROL_STAGE_DATA:
            return communication::processBinaryCommand(static_cast<communication::command>(request->bRequest), communication::direction::SET, request->wIndex, request->wValue, messageBuffer, &messageLength);
            break;

          case CONTROL_STAGE_ACK:
            return true;
          
          default:
            break;
        }
      }
      
      
      // Class request in the vendor specific range (0x80 - 0xff)
      //__ASM volatile("bkpt");
      break;

    case TUSB_REQ_TYPE_CLASS:
      // Class request in the CDC range (0x20 - 0x2f)
      __ASM volatile("bkpt");
      break;

    default: break;
  }

  // stall unknown request
  return false;
}
