
#ifndef BLE_H
#define BLE_H

#include <string>
#include <stdint.h>

namespace Bluetooth{

    enum class AdvertisementType : uint8_t {
        CONNECTABLE = 0,
        BEACON = 1,
        CONNECTABLE_AND_BEACON = 2
    };

    enum class Authentication: uint8_t {
        AUTOMATIC_WITH_BODING = 0,
        YES_NO = 1,
        AUTOMATIC = 2,
        ENTER_PASSKEY = 3,
        DISPLAY_PASSKEY = 4,
        ENTER_PASSKEY_ON_BOTH = 5
    };

    enum class CommandResponse : uint8_t {
        OK = 0,
        ERROR = 1,
        TIMEOUT = 2
    };

    enum class Mode : uint8_t {
        COMMAND = 0,
        DATA = 1
    };

    enum class StatusMessage {
        ADV_TIMEOUT, // Advertisement timeout, if advertisement time is specified by command A
        BONDED, // Security materials such as Link Key are saved into NVM
        CONN_PARAM, // Update connection parameters (interval, latency, timeout)
        CONNECT, // Connect to BLE device with address <Addr>
        DISCONNECT, // BLE connection lost
        ERR_CONNPARAM, // Failed to update connection parameters
        ERR_MEMORY, // Running out of dynamic memory
        ERR_READ, // Failed to read characteristic value
        ERR_RMT_CMD, // Failed to start remote command (insecure link or mismatched PIN)
        ERR_SEC, // Failed to secure the BLE link
        KEY, // Display the 6-digit security key
        KEY_REQ, // Request input of security key
        INDI, // Received value indication for characteristic handle
        NOTI, // Received value notification for characteristic handle
        PIO1H, // PIO1 rising edge event
        PIO1L, // PIO1 falling edge event
        PIO2H, // PIO2 rising edge event
        PIO2L, // PIO2 falling edge event
        PIO3H, // PIO3 rising edge event
        PIO3L, // PIO3 falling edge event
        RE_DISCV, // Received data indication of service changed, redo service discovery
        REBOOT, // Reboot finished
        RMT_CMD_OFF, // End of Remote Command mode
        RMT_CMD_ON, // Start of Remote Command mode
        RV, // Read value for characteristic handle
        S_RUN, // Debug info when running script (command called by script)
        SECURED, // BLE link is secured
        STREAM_OPEN, // UART transparent data pipe established
        TMR1, // Timer 1 expired
        TMR2, // Timer 2 expired
        TMR3, // Timer 3 expired
        WC, // Received start/end notification/indication request
        WV, // Received write request for characteristic handle
        ADV_CONNECTABLE, // Received connectable advertisement
        ADV_NON_CONNECTABLE, // Received non-connectable advertisement
        UNKNOWN // Unrecognized message
    };

    uint8_t init(void);
    uint8_t _send_string(std::string data);
    void _clear_rx_buffer(void);

}


#endif