
#ifndef BLE_H
#define BLE_H

#include <string>
#include <stdint.h>
#include <memory>	
#include "stmcpp/units.hpp"
#include "stmcpp/register.hpp"
#include "stmcpp/gpio.hpp"
#include "stmcpp/systick.hpp"
#include "protocol/parser.hpp"
#include "protocol/codec.hpp"

namespace Bluetooth{

    using namespace stmcpp::units;

    static constexpr stmcpp::units::duration DEFAULT_TIMEOUT = 1000_ms;
	static constexpr int BUFF_SIZE = 256;

	static constexpr char STATUS_DELIMITER = '%';

	// Pairing with the controller (Remote): connects only to a device with this name (SN in ../midicontroller-fw)
	static constexpr const char *REMOTE_NAME = "MIDIControlRemote";
	// Total time from startPairing() to DONE; after that X/Z/K,1 and FAILED
	static constexpr stmcpp::units::duration PAIRING_TIMEOUT = 15000_ms;

	enum class PairingState : uint8_t {
		IDLE = 0,       // nothing in progress
		SCANNING = 1,   // Z, F: waiting for an advertisement from REMOTE_NAME
		CONNECTING = 2, // X, C,<type>,<address>: waiting for %CONNECT%
		BONDING = 3,    // B: waiting for %SECURED% / %BONDED%
		DONE = 4,       // paired
		FAILED = 5      // timeout, disconnect or security error
	};
	static constexpr char MESSAGE_DELIMITER = '\r';


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
                ERR_CONN, // Connection attempt cancelled (Z) or failed
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
    

    // USART2/DMA initialization and module reset. Blocking (max. DEFAULT_TIMEOUT), called only at startup
    // (before USB). Later module restarts are done by the non-blocking restart().
    uint8_t init(void);

    // Non-blocking module restart via RST (RST low for 100 ms, then high, wait for %REBOOT% ≤ DEFAULT_TIMEOUT),
    // performed by process(). Result: restartState() SUCCEEDED/FAILED, acknowledge with restartAck().
    enum class RestartState : uint8_t { IDLE, RUNNING, SUCCEEDED, FAILED };
    void restart();
    RestartState restartState();
    void restartAck();

    // State of the configuration queued by configure(): DONE only with %REBOOT% after "R,1"; FAILED on a "$$$" timeout / missing %REBOOT%
    enum class ConfigState : uint8_t { NONE, PENDING, DONE, FAILED };
    ConfigState configState();
    // Module is silent: 3 consecutive "$$$" timeouts
    bool wedged();
    // The main loop reports whether BLE is usable (menu: pairing when BLE is down)
    bool isUp();
    void setUp(bool up);

    // Queues the module configuration (SN, SA, SR, SS, R,1). Non-blocking — performed by process().
    uint8_t configure();

    // Queues an attempt to connect to the last bonded device (Z + C). Non-blocking.
    uint8_t reconnect();

    // Main loop handler: parsing of received data, command-mode state machine.
    void process();

    // Module status messages (%...%), parsed in the main loop
    bool isCommandAvailable();
    std::unique_ptr<Command> getCommand();

    // Queues one command (without \r) into the command queue. Returns false when the queue is full.
    bool queueCommand(const char *command);

    // First pairing with the controller (from the menu): [K,1], Z, F and the SCANNING state. The rest of the state machine
    // (connect, B, timeout) is driven by the main loop from the status messages. Returns 1 when pairing is already running or the command queue is full.
    uint8_t startPairing();
    // Stops scanning (X). false = command queue full
    bool stopScan();
    // Connect to the address from the advertisement: C,<address type 0/1>,<12 hex characters>. false = command queue full
    bool connectTo(uint8_t addrType, const char *addr);
    // Address of the last connectTo() (12 hex characters; "" before the first call)
    const char *pendingConnectAddr();

    PairingState pairingState();
    void setPairingState(PairingState state);
    // Time since startPairing()
    stmcpp::units::duration pairingElapsed();
    // SCANNING, CONNECTING or BONDING
    bool isPairing();

    // true if the command-mode state machine is doing nothing and the module is in data mode
    bool isIdle();

    // Connection state
    bool isConnected();
    void setConnected(bool connected);
    bool isBonded();
    void setBonded(bool bonded);
    bool isStreamOpen();
    void setStreamOpen(bool open);

    // Accepted peer: link secured (%SECURED%/%BONDED%) with a device whose address from %CONNECT% matched
    // the stored controller (or the pairing address). Set by the main loop; without it received frames are dropped.
    bool isPeerAccepted();
    void setPeerAccepted();
    // Revoke the peer acceptance and drop received frames and the parser state (%CONNECT%, %DISCONNECT%, %REBOOT%, restart()).
    // setConnected(false) calls it by itself.
    void resetPeer();

    // Can a data frame be sent to the Remote now? (link + transparent UART + accepted peer + data mode)
    bool canSendData();

    // Data receive timestamp
    stmcpp::units::duration getDataReceivedTimestamp();

    // Protocol packets — queue of received packets (filled by process())
    bool isProtocolPacketAvailable();
    bool getProtocolPacket(protocol::Packet &pkt);

    // Queues a packet into the TX queue (DMA). Returns false when it cannot be sent (no link / queue full).
    bool sendProtocolPacket(const protocol::Packet &pkt);

}


#endif