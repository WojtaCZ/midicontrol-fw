
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

	// Párování s ovladačem (Remote): připojuje se jen k zařízení s tímto jménem (SN v ../midicontroller-fw)
	static constexpr const char *REMOTE_NAME = "MIDIControlRemote";
	// Celková doba od startPairing() do DONE; potom X/Z/K,1 a FAILED
	static constexpr stmcpp::units::duration PAIRING_TIMEOUT = 15000_ms;

	enum class PairingState : uint8_t {
		IDLE = 0,       // nic neprobíhá
		SCANNING = 1,   // Z, F: čekáme na reklamu REMOTE_NAME
		CONNECTING = 2, // X, C,<typ>,<adresa>: čekáme na %CONNECT%
		BONDING = 3,    // B: čekáme na %SECURED% / %BONDED%
		DONE = 4,       // spárováno
		FAILED = 5      // timeout, odpojení nebo chyba zabezpečení
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
    

    // Inicializace USART2/DMA a reset modulu. Blokující (max. DEFAULT_TIMEOUT), volá se jen při startu
    // (před USB). Pozdější restarty modulu dělá neblokující restart().
    uint8_t init(void);

    // Neblokující restart modulu přes RST (RST dole 100 ms, nahoru, čekání na %REBOOT% ≤ DEFAULT_TIMEOUT),
    // provádí ho process(). Výsledek: restartState() SUCCEEDED/FAILED, potvrdit restartAck().
    enum class RestartState : uint8_t { IDLE, RUNNING, SUCCEEDED, FAILED };
    void restart();
    RestartState restartState();
    void restartAck();

    // Stav konfigurace zařazené configure(): DONE až s %REBOOT% po "R,1"; FAILED při timeoutu "$$$" / chybějícím %REBOOT%
    enum class ConfigState : uint8_t { NONE, PENDING, DONE, FAILED };
    ConfigState configState();
    // Modul mlčí: 3 po sobě jdoucí timeouty "$$$"
    bool wedged();
    // Hlavní smyčka hlásí, zda je BLE použitelné (menu: párování při výpadku BLE)
    bool isUp();
    void setUp(bool up);

    // Zařadí konfiguraci modulu (SN, SA, SR, SS, R,1). Neblokující — provádí ji process().
    uint8_t configure();

    // Zařadí pokus o připojení k poslednímu bondovanému zařízení (Z + C). Neblokující.
    uint8_t reconnect();

    // Obsluha v hlavní smyčce: parsování přijatých dat, stavový automat příkazového režimu.
    void process();

    // Stavové zprávy modulu (%...%), parsované v hlavní smyčce
    bool isCommandAvailable();
    std::unique_ptr<Command> getCommand();

    // Zařadí jeden příkaz (bez \r) do příkazové fronty. Vrací false, když je fronta plná.
    bool queueCommand(const char *command);

    // První spárování s ovladačem (z menu): [K,1], Z, F a stav SCANNING. Zbytek automatu (připojení, B, timeout)
    // řídí hlavní smyčka podle stavových zpráv. Vrací 1, když párování už běží nebo je fronta příkazů plná.
    uint8_t startPairing();
    // Zastaví skenování (X). false = fronta příkazů plná
    bool stopScan();
    // Připojení k adrese z reklamy: C,<typ adresy 0/1>,<12 hex znaků>. false = fronta příkazů plná
    bool connectTo(uint8_t addrType, const char *addr);
    // Adresa posledního connectTo() (12 hex znaků; "" před prvním voláním)
    const char *pendingConnectAddr();

    PairingState pairingState();
    void setPairingState(PairingState state);
    // Čas od startPairing()
    stmcpp::units::duration pairingElapsed();
    // SCANNING, CONNECTING nebo BONDING
    bool isPairing();

    // true, pokud automat příkazového režimu nic neprovádí a modul je v datovém režimu
    bool isIdle();

    // Connection state
    bool isConnected();
    void setConnected(bool connected);
    bool isBonded();
    void setBonded(bool bonded);
    bool isStreamOpen();
    void setStreamOpen(bool open);

    // Přijatý protějšek: spojení zabezpečené (%SECURED%/%BONDED%) se zařízením, jehož adresa z %CONNECT% odpovídala
    // uloženému ovladači (nebo adrese párování). Nastavuje hlavní smyčka; bez něj se přijaté rámce zahazují.
    bool isPeerAccepted();
    void setPeerAccepted();
    // Zrušit přijetí protějšku a zahodit přijaté rámce i stav parseru (%CONNECT%, %DISCONNECT%, %REBOOT%, restart()).
    // setConnected(false) ho volá samo.
    void resetPeer();

    // Lze teď odeslat datový rámec k Remote? (spojení + transparentní UART + přijatý protějšek + datový režim)
    bool canSendData();

    // Data receive timestamp
    stmcpp::units::duration getDataReceivedTimestamp();

    // Protokolové pakety — fronta přijatých paketů (plní process())
    bool isProtocolPacketAvailable();
    bool getProtocolPacket(protocol::Packet &pkt);

    // Zařadí paket do TX fronty (DMA). Vrací false, když nelze odeslat (není spojení / plná fronta).
    bool sendProtocolPacket(const protocol::Packet &pkt);

}


#endif