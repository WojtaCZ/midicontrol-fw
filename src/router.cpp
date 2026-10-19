#include "router.hpp"
#include "bluetooth.hpp"
#include "display.hpp"
#include "menu.hpp"
#include "base.hpp"
#include "led.hpp"
#include "ringbuffer.hpp"
#include "debug.hpp"

#include <stm32g431xx.h>
#include <core_cm4.h>
#include <cmsis_compiler.h>
#include <cstring>

#include <tusb.h>
#include <tinyusb/src/class/cdc/cdc_device.h>

#include "stmcpp/register.hpp"
#include "stmcpp/systick.hpp"

#include "protocol/parser.hpp"
#include "protocol/codec.hpp"
#include "protocol/constants.hpp"

using namespace stmcpp::units;

namespace router {

	using protocol::MsgType;
	using protocol::NakReason;
	using protocol::Packet;

	// Časování podle lib/protocol (constants.hpp)
	static constexpr duration RETRY_TIMEOUT = duration::fromMilliSeconds(protocol::RETRY_TIMEOUT_MS);
	static constexpr duration KEEPALIVE_PERIOD = duration::fromMilliSeconds(protocol::KEEPALIVE_PERIOD_MS);
	static constexpr duration KEEPALIVE_LOSS = duration::fromMilliSeconds(protocol::KEEPALIVE_LOSS_MS);

	// ── Stav ──────────────────────────────────────────────────────────────

	// Jeden čítač PacketId pro všechny zprávy, které Base posílá
	static uint8_t _packetId = 0;

	// USB-CDC: parser příjmu a odesílací buffer (krátké zápisy do TinyUSB se dopisují v process())
	static protocol::Parser _cdcParser;
	static RingBuffer<uint8_t, 1024> _cdcTx;

	// Dostupnost protistran
	static duration _lastPcRx = 0_ms;
	static bool _pcEverSeen = false;
	// used: testy (tools/basetest, conftest.remote_alive) z nich počítají isRemoteAlive() přes sondu
	[[gnu::used]] static duration _lastRemoteRx = 0_ms;
	[[gnu::used]] static bool _remoteEverSeen = false;
	static duration _lastKeepalive = 0_ms;

	// Diagnostika (log): poslední hlášený stav protistran, zahozené rámce a plný CDC buffer
	static bool _pcAliveLogged = false;
	static bool _remoteAliveLogged = false;
	static uint16_t _badFrames = 0;
	static uint16_t _cdcOverflows = 0;
	static duration _lastCountReport = 0_ms;

	// Nepotvrzené pakety, které Base sama odeslala (čekají na ACK/NAK)
	struct Pending {
		Packet pkt;
		duration sentAt;
		uint8_t retries;
		bool used;
	};
	static constexpr uint8_t MAX_PENDING = 4;
	static Pending _pending[MAX_PENDING] = {};

	// Deduplikace: poslední PacketId zpracovaný lokálně od každého uzlu (index = adresa)
	struct LastRx {
		uint8_t packetId;
		bool valid;
	};
	static LastRx _lastRx[4] = {};

	// Restart do bootloaderu (ENTER_BOOTLOADER)
	static constexpr uint32_t kEnterBootMagic = 0xB00710AD; // must match the bootloader
	static bool _enterBootloader = false;
	static duration _enterBootloaderAt = 0_ms;

	// ── Pomocné funkce ────────────────────────────────────────────────────

	// Zprávy, které se nepotvrzují (PROTOCOL.md: Reliability)
	static bool needsAck(const Packet &pkt) {
		if (pkt.dest == protocol::ADDR_BROADCAST) return false;
		switch (pkt.msgType) {
			case MsgType::ACK:
			case MsgType::NAK:
			case MsgType::KEEPALIVE:
			case MsgType::KEEPALIVE_ACK:
			case MsgType::NOW_PLAYING_INFO:
				return false;
			default:
				return true;
		}
	}

	static Transport transportFor(uint8_t dest) {
		return (dest == protocol::ADDR_REMOTE) ? Transport::BLE : Transport::USB_CDC;
	}

	bool isPcAlive() {
		return _pcEverSeen && (stmcpp::systick::getDuration() - _lastPcRx) < KEEPALIVE_LOSS;
	}

	bool isRemoteAlive() {
		return Bluetooth::isConnected() && _remoteEverSeen
			   && (stmcpp::systick::getDuration() - _lastRemoteRx) < KEEPALIVE_LOSS;
	}

	static bool pcReachable() {
		return tud_mounted() && (isPcAlive() || tud_cdc_connected());
	}

	static bool sendCdc(const Packet &pkt) {
		uint8_t frame[protocol::MAX_FRAME_SIZE];
		uint16_t len = protocol::encode(pkt, frame);
		if (len == 0 || !tud_mounted()) return false;
		if (_cdcTx.freeSpace() < len) { // Celý rámec, nebo nic
			if (_cdcOverflows != 0xFFFF) _cdcOverflows++;
			return false;
		}

		for (uint16_t i = 0; i < len; ++i) {
			_cdcTx.push(frame[i]);
		}
		LED::notifyActivity(LED::PIXEL_USB);
		return true;
	}

	static bool transmit(const Packet &pkt, Transport via) {
		if (via == Transport::USB_CDC) return sendCdc(pkt);
		return Bluetooth::sendProtocolPacket(pkt);
	}

	// Odeslání do USB po částech podle volného místa v TinyUSB FIFO
	static void flushCdc() {
		if (!tud_mounted()) {
			_cdcTx.clear();
			return;
		}

		bool wrote = false;
		while (!_cdcTx.empty()) {
			uint32_t avail = tud_cdc_write_available();
			if (avail == 0) break;

			size_t len = 0;
			const uint8_t *data = _cdcTx.contiguousData(len);
			if (len > avail) len = avail;

			uint32_t written = tud_cdc_write(data, static_cast<uint32_t>(len));
			if (written == 0) break;
			_cdcTx.consume(written);
			wrote = true;
		}

		if (wrote) tud_cdc_write_flush();
	}

	static void sendSimple(uint8_t dest, MsgType type, const uint8_t *payload, uint8_t len, Transport via) {
		Packet pkt = {};
		pkt.source = protocol::ADDR_BASE;
		pkt.dest = dest;
		pkt.packetId = _packetId++;
		pkt.msgType = type;
		if (len > protocol::MAX_PAYLOAD_SIZE) len = protocol::MAX_PAYLOAD_SIZE;
		if (len) memcpy(pkt.payload, payload, len);
		pkt.payloadLen = len;
		transmit(pkt, via);
	}

	static void sendAck(const Packet &rx, Transport via) {
		uint8_t payload[1] = {rx.packetId};
		sendSimple(rx.source, MsgType::ACK, payload, 1, via);
	}

	static void sendNak(const Packet &rx, NakReason reason, Transport via) {
		Debug::log(Debug::Level::INFO, "router", "NAK 0x%02x for type 0x%02x from 0x%02x pid %u", static_cast<unsigned>(reason), static_cast<unsigned>(rx.msgType),
				   rx.source, rx.packetId);
		uint8_t payload[2] = {rx.packetId, static_cast<uint8_t>(reason)};
		sendSimple(rx.source, MsgType::NAK, payload, 2, via);
	}

	bool sendReliable(Packet &pkt) {
		pkt.source = protocol::ADDR_BASE;

		for (auto &p : _pending) {
			if (p.used) continue;

			pkt.packetId = _packetId++;
			Transport via = transportFor(pkt.dest);
			if (!transmit(pkt, via)) return false;

			p.pkt = pkt;
			p.sentAt = stmcpp::systick::getDuration();
			p.retries = 0;
			p.used = true;
			return true;
		}
		return false; // Fronta plná
	}

	// ACK/NAK od cílového uzlu uzavře odpovídající nepotvrzený paket
	static void resolvePending(const Packet &rx) {
		if (rx.payloadLen < 1) return;
		uint8_t ackedId = rx.payload[0];
		for (auto &p : _pending) {
			if (p.used && p.pkt.dest == rx.source && p.pkt.packetId == ackedId) {
				p.used = false;
			}
		}
	}

	static void retryPending() {
		duration now = stmcpp::systick::getDuration();
		for (auto &p : _pending) {
			if (!p.used || (now - p.sentAt) < RETRY_TIMEOUT) continue;

			if (p.retries >= protocol::MAX_RETRIES) {
				Debug::log(Debug::Level::WARNING, "router", "retries exhausted: type 0x%02x to 0x%02x pid %u", static_cast<unsigned>(p.pkt.msgType),
						   p.pkt.dest, p.pkt.packetId);
				p.used = false; // Vzdáváme to; GUI má vlastní timeouty
				continue;
			}
			// Stejné PacketId — příjemce duplikát potvrdí, ale neprovede dvakrát
			transmit(p.pkt, transportFor(p.pkt.dest));
			p.retries++;
			p.sentAt = now;
		}
	}

	static void sendKeepaliveToRemote() {
		duration now = stmcpp::systick::getDuration();
		if ((now - _lastKeepalive) < KEEPALIVE_PERIOD) return;
		_lastKeepalive = now;
		if (!Bluetooth::canSendData()) return;
		sendSimple(protocol::ADDR_REMOTE, MsgType::KEEPALIVE, nullptr, 0, Transport::BLE);
	}

	static void requestBootloader() {
		// Set the backup-register flag the bootloader checks on reset, so it stays
		// resident (USB-CDC firmware update) instead of jumping back to the app.
		stmcpp::reg::set(std::ref(RCC->APB1ENR1), RCC_APB1ENR1_PWREN);
		PWR->CR1 |= PWR_CR1_DBP; // unlock backup domain write protection
		TAMP->BKP0R = kEnterBootMagic;
	}

	// Kopie ASCII payloadu do řetězce (bez koncové nuly v payloadu, případná nula se toleruje)
	template<size_t N>
	static void payloadToString(const Packet &pkt, char (&out)[N]) {
		size_t len = pkt.payloadLen < N - 1 ? pkt.payloadLen : N - 1;
		memcpy(out, pkt.payload, len);
		out[len] = '\0';
	}

	// Signály PC → Remote; Base je sleduje, aby OLED odpovídal stavu přehrávání
	static void observeSignal(const Packet &pkt) {
		switch (pkt.msgType) {
			case MsgType::SIGNAL_PLAYING: {
				char code[protocol::MAX_SONG_CODE_LEN + 1];
				payloadToString(pkt, code);
				GUI::showNowPlaying(code);
				break;
			}
			case MsgType::SIGNAL_STOPPED:
				GUI::dismissNowPlaying();
				break;
			case MsgType::SIGNAL_ANCHOR: {
				char label[32];
				payloadToString(pkt, label);
				GUI::setNowPlayingAnchor(label);
				break;
			}
			default:
				break;
		}
	}

	// ── Lokální obsluha paketů pro Base ───────────────────────────────────

	static void handleLocal(const Packet &pkt, Transport via) {
		bool ack = needsAck(pkt);

		// Deduplikace opakovaných paketů: potvrdit znovu, ale neprovádět
		if (ack && pkt.source < 4) {
			LastRx &last = _lastRx[pkt.source];
			if (last.valid && last.packetId == pkt.packetId) {
				sendAck(pkt, via);
				return;
			}
			last.packetId = pkt.packetId;
			last.valid = true;
		}

		switch (pkt.msgType) {
			case MsgType::SET_CURRENT:
				if (pkt.payloadLen < 1) {
					sendNak(pkt, NakReason::INVALID_PAYLOAD, via);
					break;
				}
				base::current::set(pkt.payload[0] != 0);
				GUI::syncPowerCheckbox(); // Checkbox v menu podle skutečného stavu SSR
				if (ack) sendAck(pkt, via);
				break;

			case MsgType::GET_STATUS: {
				if (ack) sendAck(pkt, via);
				Packet resp = {};
				resp.dest = pkt.source;
				resp.msgType = MsgType::STATUS_RESP;
				resp.payload[0] = static_cast<uint8_t>(Bluetooth::isConnected());
				resp.payload[1] = static_cast<uint8_t>(base::current::isEnabled());
				resp.payload[2] = static_cast<uint8_t>(display::isConnected());
				resp.payload[3] = static_cast<uint8_t>(isPcAlive());
				resp.payloadLen = 4;
				sendReliable(resp); // Odpověď se také potvrzuje
				break;
			}

			case MsgType::NOW_PLAYING_INFO:
				// Jeden rámec na displej, bez ACK (častá zpráva)
				display::applyNowPlaying(pkt.payload, pkt.payloadLen);
				GUI::updateNowPlayingInfo();
				break;

			case MsgType::SONG_LIST_RESP:
				if (ack) sendAck(pkt, via);
				if (pkt.payloadLen >= 1) {
					bool hasMore = (pkt.payload[0] != 0x00);
					char songList[protocol::MAX_PAYLOAD_SIZE];
					uint8_t len = pkt.payloadLen - 1;
					if (len > protocol::MAX_PAYLOAD_SIZE - 1) len = protocol::MAX_PAYLOAD_SIZE - 1;
					memcpy(songList, &pkt.payload[1], len);
					songList[len] = '\0';
					GUI::handleSongListResponse(songList, len, hasMore);
				}
				break;

			case MsgType::SIGNAL_PLAYING:
			case MsgType::SIGNAL_STOPPED:
			case MsgType::SIGNAL_ANCHOR:
			case MsgType::SIGNAL_RECORDING:
				// Signály adresované přímo Base (např. když Remote není připojen)
				observeSignal(pkt);
				if (ack) sendAck(pkt, via);
				break;

			case MsgType::KEEPALIVE:
				sendSimple(pkt.source, MsgType::KEEPALIVE_ACK, nullptr, 0, via);
				break;

			case MsgType::KEEPALIVE_ACK:
				// Dostupnost se aktualizuje při každém přijatém paketu (processPacket)
				break;

			case MsgType::ACK:
			case MsgType::NAK:
				resolvePending(pkt);
				break;

			case MsgType::ENTER_BOOTLOADER:
				// Acknowledge, then reboot into the USB bootloader shortly afterwards
				// (the delay lets the ACK reach the PC before the device re-enumerates).
				sendAck(pkt, via);
				Debug::log(Debug::Level::INFO, "ota", "ENTER_BOOTLOADER from 0x%02x, reset in 250 ms", pkt.source);
				requestBootloader();
				_enterBootloader = true;
				_enterBootloaderAt = stmcpp::systick::getDuration() + 250_ms;
				break;

			default:
				// Neznámé typy i příkazy určené PC (PLAY_SONG, GET_SONG_LIST, ...) Base neobsluhuje
				if (ack) sendNak(pkt, NakReason::UNKNOWN_MSG, via);
				break;
		}
	}

	// ── Směrování ─────────────────────────────────────────────────────────

	static void forward(const Packet &pkt, Transport from) {
		Transport to = (from == Transport::USB_CDC) ? Transport::BLE : Transport::USB_CDC;

		bool reachable = (to == Transport::BLE) ? Bluetooth::canSendData() : pcReachable();
		if (reachable && transmit(pkt, to)) {
			if (to == Transport::BLE) LED::notifyActivity(LED::PIXEL_BLUETOOTH);
			return;
		}

		// Nelze předat: odesílatel přestane opakovat (NAK se neposílá na nepotvrzované zprávy)
		if (!needsAck(pkt)) return;
		bool linkUp = (to == Transport::BLE) ? Bluetooth::isConnected() : pcReachable();
		sendNak(pkt, linkUp ? NakReason::BUSY : NakReason::UNREACHABLE, from);
	}

	static void processPacket(const Packet &pkt, Transport via) {
		duration now = stmcpp::systick::getDuration();
		Debug::log(Debug::Level::DEBUG, "router", "rx %s 0x%02x->0x%02x pid %u type 0x%02x len %u", via == Transport::USB_CDC ? "cdc" : "ble", pkt.source, pkt.dest,
				   pkt.packetId, static_cast<unsigned>(pkt.msgType), pkt.payloadLen);
		if (via == Transport::USB_CDC) {
			_lastPcRx = now;
			_pcEverSeen = true;
		} else {
			_lastRemoteRx = now;
			_remoteEverSeen = true;
		}

		switch (pkt.dest) {
			case protocol::ADDR_BASE:
				handleLocal(pkt, via);
				break;

			case protocol::ADDR_BROADCAST:
				handleLocal(pkt, via);
				forward(pkt, via);
				break;

			case protocol::ADDR_REMOTE:
				if (via != Transport::USB_CDC) break; // Remote sám sobě — zahodit
				observeSignal(pkt);                  // Base nekonzumuje, jen sleduje (OLED)
				forward(pkt, via);
				break;

			case protocol::ADDR_PC:
				if (via != Transport::BLE) break;
				forward(pkt, via);
				break;

			default:
				break; // Neznámá adresa
		}
	}

	// Změny dostupnosti PC/Remote a souhrn zahozených rámců (nejvýše jednou za sekundu) do logu
	static void logChanges() {
		bool pc = isPcAlive();
		if (pc != _pcAliveLogged) {
			_pcAliveLogged = pc;
			Debug::log(pc ? Debug::Level::INFO : Debug::Level::WARNING, "router", "PC %s", pc ? "alive" : "lost");
		}
		bool remote = isRemoteAlive();
		if (remote != _remoteAliveLogged) {
			_remoteAliveLogged = remote;
			Debug::log(remote ? Debug::Level::INFO : Debug::Level::WARNING, "router", "remote %s", remote ? "alive" : "lost");
		}
		duration now = stmcpp::systick::getDuration();
		if ((_badFrames || _cdcOverflows) && (now - _lastCountReport) >= 1000_ms) {
			_lastCountReport = now;
			if (_badFrames) Debug::log(Debug::Level::WARNING, "router", "cdc: %u bad frames dropped", static_cast<unsigned>(_badFrames));
			if (_cdcOverflows) Debug::log(Debug::Level::WARNING, "router", "cdc: tx buffer full, %u frames dropped", static_cast<unsigned>(_cdcOverflows));
			_badFrames = 0;
			_cdcOverflows = 0;
		}
	}

	void process() {
		// Příjem z USB-CDC
		if (tud_cdc_available()) {
			uint8_t buf[64];
			uint32_t count = tud_cdc_read(buf, sizeof(buf));
			LED::notifyActivity(LED::PIXEL_USB);
			for (uint32_t i = 0; i < count; ++i) {
				if (_cdcParser.feed(buf[i])) {
					processPacket(_cdcParser.getPacket(), Transport::USB_CDC);
				} else if (buf[i] == protocol::FRAME_END && _badFrames != 0xFFFF) {
					_badFrames++; // FRAME_END bez platného rámce: CRC, délka nebo neúplný rámec
				}
			}
		}

		// Příjem z BLE (fronta paketů plněná Bluetooth::process)
		Packet pkt;
		while (Bluetooth::getProtocolPacket(pkt)) {
			LED::notifyActivity(LED::PIXEL_BLUETOOTH);
			processPacket(pkt, Transport::BLE);
		}

		retryPending();
		sendKeepaliveToRemote();
		flushCdc();
		logChanges();

		// Pending reboot into the USB bootloader (after the ACK has been flushed).
		if (_enterBootloader && stmcpp::systick::getDuration() >= _enterBootloaderAt) {
			tud_cdc_write_flush();
			NVIC_SystemReset();
		}
	}

} // namespace router
