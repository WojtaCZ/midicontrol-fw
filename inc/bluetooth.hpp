
#ifndef BLE_H
#define BLE_H

#include <string>
#include <stdint.h>
#include <deque>
#include <memory>	
#include "stmcpp/units.hpp"
#include "stmcpp/register.hpp"
#include "stmcpp/gpio.hpp"
#include "stmcpp/systick.hpp"

namespace Bluetooth{

    using namespace stmcpp::units;

    static constexpr stmcpp::units::duration DEFAULT_TIMEOUT = 1000_ms;
	static constexpr int BUFF_SIZE = 256;

	static constexpr char STATUS_DELIMITER = '%';
	static constexpr char MESSAGE_DELIMITER = '\r';
	static constexpr std::string OK_STRING = "AOK";
	static constexpr std::string ERROR_STRING = "ERR";
	static constexpr std::string CMD_STRING = "CMD> ";


    enum class AdvertisementType : uint8_t {
        CONNECTABLE = 0,
        BEACON = 1,
        CONNECTABLE_AND_BEACON = 2
    };

    enum class CommandResponse : uint8_t {
        OK = 0,
        ERROR = 1,
        NONE = 2
    };

    enum class Mode : uint8_t {
        COMMAND = 0,
        DATA = 1
    };

    class Command {
        public:
            enum class Type {
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

            Command(Type type): _type(type) {}
            virtual ~Command() = default; // Virtual destructor

            Type getType() const {
                return _type;
            }

        private:
            Type _type;
    };

    class ConnParamCommand : public Command {
        public:
            ConnParamCommand(int interval_, int latency_, int timeout_)
            : Command(Type::CONN_PARAM), interval(interval_), latency(latency_), timeout(timeout_) {}
            int getInterval() const { return interval; }
            int getLatency() const { return latency; }
            int getTimeout() const { return timeout; }
        private:
            int interval;
            int latency;
            int timeout;
    };

    class ConnectCommand : public Command {
        public:
            ConnectCommand(int connected_, const std::string &addr_)
            : Command(Type::CONNECT), connected(connected_), addr(addr_) {}
            int getConnected() const { return connected; }
            const std::string &getAddr() const { return addr; }
        private:
            int connected;
            std::string addr;
    };

    class KeyCommand : public Command {
        public:
            KeyCommand(int key_)
            : Command(Type::KEY), key(key_) {}
            int getKey() const { return key; }
        private:
            int key;
    };

    class IndiCommand : public Command {
        public:
            IndiCommand(int hdl_, const std::string &hex_)
            : Command(Type::INDI), hdl(hdl_), hex(hex_) {}
            int getHandle() const { return hdl; }
            const std::string &getHex() const { return hex; }
        private:
            int hdl;
            std::string hex;
    };

    class NotiCommand : public Command {
        public:
            NotiCommand(int hdl_, const std::string &hex_)
            : Command(Type::NOTI), hdl(hdl_), hex(hex_) {}
            int getHandle() const { return hdl; }
            const std::string &getHex() const { return hex; }
        private:
            int hdl;
            std::string hex;
    };

    class RvCommand : public Command {
        public:
            RvCommand(int hdl_, const std::string &hex_)
            : Command(Type::RV), hdl(hdl_), hex(hex_) {}
            int getHandle() const { return hdl; }
            const std::string &getHex() const { return hex; }
        private:
            int hdl;
            std::string hex;
    };

    class SRunCommand : public Command {
        public:
            SRunCommand(const std::string &cmd_)
            : Command(Type::S_RUN), cmd(cmd_) {}
            const std::string &getCmd() const { return cmd; }
        private:
            std::string cmd;
    };

    class WcCommand : public Command {
        public:
            WcCommand(int hdl_, const std::string &hex_)
            : Command(Type::WC), hdl(hdl_), hex(hex_) {}
            int getHandle() const { return hdl; }
            const std::string &getHex() const { return hex; }
        private:
            int hdl;
            std::string hex;
    };

    class WvCommand : public Command {
        public:
            WvCommand(int hdl_, const std::string &hex_)
            : Command(Type::WV), hdl(hdl_), hex(hex_) {}
            int getHandle() const { return hdl; }
            const std::string &getHex() const { return hex; }
        private:
            int hdl;
            std::string hex;
    };

    class AdvConnectableCommand : public Command {
        public:
            AdvConnectableCommand(const std::string &addr_, int connected_, const std::string &name_, const std::string &uuids_, int rssi_)
            : Command(Type::ADV_CONNECTABLE), addr(addr_), connected(connected_), name(name_), uuids(uuids_), rssi(rssi_) {}
            const std::string &getAddr() const { return addr; }
            int getConnected() const { return connected; }
            const std::string &getName() const { return name; }
            const std::string &getUuids() const { return uuids; }
            int getRssi() const { return rssi; }
        private:
            std::string addr;
            int connected;
            std::string name;
            std::string uuids;
            int rssi;
    };

    class AdvNonConnectableCommand : public Command {
        public:
            AdvNonConnectableCommand(const std::string &addr_, int connected_, int rssi_, const std::string &hex_)
            : Command(Type::ADV_NON_CONNECTABLE), addr(addr_), connected(connected_), rssi(rssi_), hex(hex_) {}
            const std::string &getAddr() const { return addr; }
            int getConnected() const { return connected; }
            int getRssi() const { return rssi; }
            const std::string &getHex() const { return hex; }
        private:
            std::string addr;
            int connected;
            int rssi;
            std::string hex;
    };
    

    uint8_t init(void);
    bool isCommandAvailable();
    std::unique_ptr<Command> getCommand();
    uint8_t _send_string(std::string data);
    void _clear_rx_buffer(void);
    Mode setMode(Mode mode);

    CommandResponse sendCommand(std::string command);
    CommandResponse sendCommand(std::string command, std::string & response, bool awaitResponse = true, stmcpp::units::duration timeout = DEFAULT_TIMEOUT);

}


#endif