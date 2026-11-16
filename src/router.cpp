#include "router.hpp"
#include "bluetooth.hpp"
#include "display.hpp"
#include "menu.hpp"
#include "base.hpp"
#include "led.hpp"
#include "ringbuffer.hpp"
#include "debug.hpp"
#include "fault.hpp"
#include "git.hpp"
#include "resetcause.hpp"
#include "settings.hpp"
#include "midi.hpp"
#include "logic/recovery.hpp"
#include "logic/ble_status.hpp"

#include <stm32g431xx.h>
#include <core_cm4.h>
#include <cmsis_compiler.h>
#include <cstdio>
#include <cstring>

#include <tusb.h>
#include <tinyusb/src/class/cdc/cdc_device.h>

#include "stmcpp/register.hpp"
#include "stmcpp/systick.hpp"

#include "protocol/parser.hpp"
#include "protocol/codec.hpp"
#include "protocol/constants.hpp"
#include "protocol/info.hpp"
#include "boot/fw_progress.hpp"
#include "logic/router_core.hpp"

using namespace stmcpp::units;

namespace router {

	using protocol::MsgType;
	using protocol::NakReason;
	using protocol::Packet;

	// Timing according to lib/protocol (constants.hpp)
	// SysTick period (main.cpp systick::enable(144_MHz, 1_ms))
	static constexpr uint32_t TICK_US = 1000;
	static constexpr duration KEEPALIVE_PERIOD = duration::fromMilliSeconds(protocol::KEEPALIVE_PERIOD_MS);
	static constexpr duration KEEPALIVE_LOSS = duration::fromMilliSeconds(protocol::KEEPALIVE_LOSS_MS);

	// ── State ─────────────────────────────────────────────────────────────

	// A single PacketId counter for all messages the Base sends
	static uint8_t _packetId = 0;

	// USB-CDC: receive parser and send buffer (short writes to TinyUSB are completed in process())
	static protocol::Parser _cdcParser;
	static RingBuffer<uint8_t, 1024> _cdcTx;

	// Progress of the Remote update (FW_UPDATE_* via the Base): tracked only for the OLED screen
	static boot::FwProgress _remoteFw(protocol::ADDR_REMOTE);

	// Milliseconds for FwProgress (systick has 1 ms resolution; uint32_t wraparound is handled in the tracker)
	static bool bleLinkUsable() {
		return blestatus::linkUsable(Bluetooth::isConnected(), Bluetooth::isStreamOpen(), Bluetooth::isPeerAccepted());
	}

	// Router clock: ms since boot as a uint32, wraps after 49.7 days; compare only by modular differences (BF-1)
	static uint32_t nowMs() {
		return routercore::msFromTicks(stmcpp::systick::getTicks(), TICK_US);
	}

	// Availability of the peers
	// [[gnu::used]]: tools/basetest reads it (test_lifecycle.py pc_alive())
	[[gnu::used]] static routercore::PeerPresence _pc(protocol::KEEPALIVE_LOSS_MS);
	// used: tests (tools/basetest, conftest.remote_alive) compute isRemoteAlive() from them through the probe
	[[gnu::used]] static duration _lastRemoteRx = 0_ms;
	[[gnu::used]] static bool _remoteEverSeen = false;
	static uint32_t _lastRemoteRxMs = 0; // nowMs() of _lastRemoteRx (stale-link guard)
	static duration _lastKeepalive = 0_ms;

	// Diagnostics (log): last reported state of the peers, dropped frames and a full CDC buffer
	static bool _pcAliveLogged = false;
	static recovery::PcLossGuard _pcLossGuard;
	static bool _remoteAliveLogged = false;
	static uint16_t _badFrames = 0;
	static uint16_t _cdcOverflows = 0;
	static duration _lastCountReport = 0_ms;

	// Unacknowledged packets that the Base itself sent (waiting for ACK/NAK)
	static routercore::RetryQueue _pending;

	// Deduplication: the last PacketId processed locally from each node
	static routercore::RxDedup _dedup;
	static routercore::NonceRelay _nonceRelay; // the nonce the remote was last told
	static routercore::RemoteBoot _remoteBoot; // boot id from the remote's KEEPALIVE_ACK (protocol v2), cleared on loss
	static routercore::PcSession _pcSession; // KEEPALIVE nonce of the PC process (protocol v2)
	static routercore::SsrBackstop _backstop; // Q1-1 c: SSR on, 2 h without a PC packet -> off

	// Restart into the bootloader (ENTER_BOOTLOADER)
	static constexpr uint32_t kEnterBootMagic = 0xB00710AD; // must match the bootloader
	static bool _enterBootloader = false;
	static uint32_t _enterBootloaderAt = 0; // nowMs()

	// ── Helper functions ──────────────────────────────────────────────────

	// Messages that are not acknowledged (PROTOCOL.md: Reliability)
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
		_pc.update(nowMs());
		return _pc.alive();
	}

	bool isRemoteAlive() {
		return Bluetooth::isConnected() && _remoteEverSeen
			   && (stmcpp::systick::getDuration() - _lastRemoteRx) < KEEPALIVE_LOSS;
	}

	uint32_t clockMs() { return nowMs(); }

	bool remoteRx(uint32_t &atMs) {
		atMs = _lastRemoteRxMs;
		return _remoteEverSeen;
	}

	bool remoteFwBusy() { return _remoteFw.state() != boot::FwProgress::State::Idle; }

	static bool pcReachable() {
		return tud_mounted() && (isPcAlive() || tud_cdc_connected());
	}

	static bool sendCdc(const Packet &pkt) {
		uint8_t frame[protocol::MAX_FRAME_SIZE];
		uint16_t len = protocol::encode(pkt, frame);
		if (len == 0 || !tud_mounted()) return false;
		if (_cdcTx.freeSpace() < len) { // The whole frame or nothing
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

	// Sending to USB in pieces according to the free space in the TinyUSB FIFO
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

	// PM-1/UI2-1: the originator learns why its request failed (the GUI ignores types it did not send)
	static void reportFailure(MsgType type, routercore::Failure f, NakReason reason = NakReason::UNKNOWN_MSG) {
		const char *text = routercore::failureText(f, reason);
		Debug::log(Debug::Level::WARNING, "router", "request type 0x%02x failed: %s", static_cast<unsigned>(type), text);
		GUI::requestFailed(type, text);
	}

	bool sendReliable(Packet &pkt) {
		pkt.source = protocol::ADDR_BASE;

		if (_pending.full()) {
			reportFailure(pkt.msgType, routercore::Failure::NotSent);
			return false;
		}

		pkt.packetId = _packetId++;
		Transport via = transportFor(pkt.dest);
		// PM-1: a failed first send (CDC buffer full, link busy) is queued anyway and retried like a lost packet
		if (!transmit(pkt, via)) Debug::log(Debug::Level::WARNING, "router", "first send of type 0x%02x failed, retrying", static_cast<unsigned>(pkt.msgType));

		return _pending.add(pkt, nowMs());
	}

	// An ACK/NAK from the destination node closes the corresponding unacknowledged packet; a NAK reason goes to the originator
	static void resolvePending(const Packet &rx) {
		routercore::RetryQueue::Closed closed{};
		if (_pending.resolve(rx, &closed) && closed.nak) reportFailure(closed.type, routercore::Failure::Nak, closed.reason);
	}

	static void retryPending() {
		_pending.poll(
			nowMs(), protocol::RETRY_TIMEOUT_MS, protocol::MAX_RETRIES,
			// Same PacketId — the receiver acknowledges the duplicate but does not execute it twice
			[](const Packet &p) { transmit(p, transportFor(p.dest)); },
			[](const Packet &p, bool busy) {
				Debug::log(Debug::Level::WARNING, "router", "retries exhausted: type 0x%02x to 0x%02x pid %u", static_cast<unsigned>(p.msgType), p.dest,
						   p.packetId);
				// PM-1: giving up is visible; PM-6: still BUSY after MAX_RETRIES -> "PC zaneprazdneno"
				if (busy) reportFailure(p.msgType, routercore::Failure::Nak, NakReason::BUSY);
				else reportFailure(p.msgType, routercore::Failure::NoAnswer);
			});
	}

	// The payload tells the remote whether the PC is alive; a change (also of the PC's session nonce) is sent at once (the remote's "Cekam na PC" splash)
	// force: a PC session nonce change, sent at once (before the frames of the new session are forwarded)
	static void sendKeepaliveToRemote(bool force = false) {
		static bool lastPc = false;
		duration now = stmcpp::systick::getDuration();
		uint8_t pc = static_cast<uint8_t>(isPcAlive());
		const uint16_t nonce = _pcSession.nonce();
		const bool nonceDue = _nonceRelay.pending(nonce);
		if (!force && (now - _lastKeepalive) < KEEPALIVE_PERIOD && (pc != 0) == lastPc && !nonceDue) return;
		_lastKeepalive = now;
		if (!Bluetooth::canSendData()) return; // a pending nonce stays pending: retried from forward() and every loop pass
		lastPc = pc != 0;
		_nonceRelay.sent(nonce);
		// [pcAlive, rssi, nonce lo, hi]: the base's own RSSI of this link (i8 dBm, 0x80 unknown) for the remote's link diagnostics,
		// the PC's session nonce (0 = none) so a restarted PC's PacketIds are not taken for duplicates; older remotes read byte 0 only
		const uint8_t payload[4] = {pc, static_cast<uint8_t>(Bluetooth::linkStats().rssiNow(stmcpp::systick::getTicks())),
									static_cast<uint8_t>(nonce), static_cast<uint8_t>(nonce >> 8)};
		sendSimple(protocol::ADDR_REMOTE, MsgType::KEEPALIVE, payload, 4, Transport::BLE);
	}

	static void requestBootloader() {
		// Set the backup-register flag the bootloader checks on reset, so it stays
		// resident (USB-CDC firmware update) instead of jumping back to the app.
		stmcpp::reg::set(std::ref(RCC->APB1ENR1), RCC_APB1ENR1_PWREN);
		PWR->CR1 |= PWR_CR1_DBP; // unlock backup domain write protection
		TAMP->BKP0R = kEnterBootMagic;
	}

	// Copy of the ASCII payload into a string (no terminating zero in the payload, a zero is tolerated if present)
	template<size_t N>
	static void payloadToString(const Packet &pkt, char (&out)[N]) {
		size_t len = pkt.payloadLen < N - 1 ? pkt.payloadLen : N - 1;
		memcpy(out, pkt.payload, len);
		out[len] = '\0';
	}

	// Signals PC → Remote; the Base observes them so the OLED matches the playback state
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

	// INFO_RESP payload (lib/protocol inc/protocol/info.hpp): firmware, uptime, reset cause, BLE and device state
	static size_t buildInfo(uint8_t *buf, size_t cap) {
		using namespace protocol::info;
		Writer w(buf, cap);
		char built[24];
		snprintf(built, sizeof(built), "%s %s UTC", git::build_date, git::build_time);
		w.str(Tag::REVISION, git::revision, 12);
		w.str(Tag::BRANCH, git::branch, 24);
		w.str(Tag::BUILT, built);
		w.u8(Tag::DIRTY, git::dirty ? 1 : 0);
		w.u8(Tag::PROTO_VER, protocol::PROTOCOL_VERSION);
		w.u32(Tag::UPTIME_MS, stmcpp::systick::getTicks());
		w.u8(Tag::RESET_CAUSE, static_cast<uint8_t>((resetcause::last() >> 24) & 0xFEU));
		const fault::Record *f = fault::saved();
		w.u8(Tag::FAULT_COUNT, f ? f->count : 0);
		uint8_t ble = 0;
		if (Bluetooth::isConnected()) ble |= BLE_CONNECTED;
		if (Bluetooth::isStreamOpen()) ble |= BLE_STREAM_OPEN;
		if (Bluetooth::isPeerAccepted()) ble |= BLE_SECURED;
		if (Bluetooth::isBonded()) ble |= BLE_BONDED;
		if (Bluetooth::isUp()) ble |= BLE_MODULE_UP;
		if (isRemoteAlive()) ble |= BLE_PEER_ALIVE;
		if (isPcAlive()) ble |= BLE_PC_ALIVE;
		w.u8(Tag::BLE_FLAGS, ble);
		w.str(Tag::BLE_PEER, settings::remoteAddr(), 12);
		w.str(Tag::BLE_NAME, Bluetooth::name());
		w.u8(Tag::BLE_PAIRING, static_cast<uint8_t>(Bluetooth::pairingState()));
		w.u8(Tag::DISPLAY, display::isConnected() ? 1 : 0);
		w.u8(Tag::SSR, base::current::isEnabled() ? 1 : 0);
		{
			// Link quality last: the payload limit drops these before anything older (the remote's payload is nearly full)
			const linkstats::InfoFields lf = Bluetooth::linkStats().infoFields(stmcpp::systick::getTicks(), Bluetooth::isConnected());
			w.bytes(Tag::LINK_RSSI, lf.rssi, sizeof(lf.rssi));
			w.bytes(Tag::LINK_PARAMS, lf.params, sizeof(lf.params));
			w.bytes(Tag::LINK_COUNTERS, lf.counters, sizeof(lf.counters));
		}
		return w.len();
	}

	// ── Local handling of packets for the Base ─────────────────────────────

	static void handleLocal(const Packet &pkt, Transport via) {
		bool ack = needsAck(pkt);

		// Deduplication of repeated packets: acknowledge again, but do not execute
		if (ack && _dedup.isRepeat(pkt.source, pkt.packetId)) {
			sendAck(pkt, via);
			return;
		}

		switch (pkt.msgType) {
			case MsgType::SET_CURRENT:
				if (pkt.payloadLen < 1) {
					sendNak(pkt, NakReason::INVALID_PAYLOAD, via);
					break;
				}
				base::current::set(pkt.payload[0] != 0);
				GUI::syncPowerCheckbox(); // Menu checkbox according to the actual SSR state
				if (ack) sendAck(pkt, via);
				break;

			case MsgType::GET_STATUS: {
				if (ack) sendAck(pkt, via);
				Packet resp = {};
				resp.dest = pkt.source;
				resp.msgType = MsgType::STATUS_RESP;
				// SYS-LINK-07: ble only once secured. Protocol v2: 7 B with the remote's boot id (A2)
				resp.payloadLen = routercore::buildStatus(resp.payload, bleLinkUsable(), base::current::isEnabled(), display::isConnected(),
														  isPcAlive(), _remoteBoot);
				sendReliable(resp); // The response is acknowledged too
				break;
			}

			case MsgType::GET_INFO: {
				if (ack) sendAck(pkt, via);
				Packet resp = {};
				resp.dest = pkt.source;
				resp.msgType = MsgType::INFO_RESP;
				resp.payloadLen = static_cast<uint8_t>(buildInfo(resp.payload, sizeof(resp.payload)));
				sendReliable(resp); // acknowledged like STATUS_RESP
				break;
			}

			case MsgType::NOW_PLAYING_INFO:
				// One frame to the display, no ACK (frequent message)
				display::applyNowPlaying(pkt.payload, pkt.payloadLen);
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
				// Signals addressed directly to the Base (e.g. when the Remote is not connected)
				observeSignal(pkt);
				if (ack) sendAck(pkt, via);
				break;

			case MsgType::KEEPALIVE:
				// PC hello: [nonce u16 LE] (empty from a v1 PC); a new nonce = a new PC process, forget its PacketIds.
				// KEEPALIVE is not ACKed, so it never passes the duplicate filter above.
				if (pkt.source == protocol::ADDR_PC && routercore::onPcKeepalive(_pcSession, _dedup, pkt.payload, pkt.payloadLen)) {
					Debug::log(Debug::Level::INFO, "router", "new PC session (nonce 0x%04x)", _pcSession.nonce());
					sendKeepaliveToRemote(true); // before any later frame of this pass is forwarded
				}
				sendSimple(pkt.source, MsgType::KEEPALIVE_ACK, nullptr, 0, via);
				break;

			case MsgType::KEEPALIVE_ACK:
				// Availability is updated on every received packet (processPacket). The remote appends its own RSSI (i8 dBm)
				if (pkt.source == protocol::ADDR_REMOTE && pkt.payloadLen >= 1) {
					Bluetooth::linkStats().setPeerRssi(static_cast<int8_t>(pkt.payload[0]), stmcpp::systick::getTicks());
				}
				// Protocol v2 remote: [rssi, boot_id u16 LE]; a v1 remote sends 1 byte and leaves the id invalid
				if (pkt.source == protocol::ADDR_REMOTE) _remoteBoot.onKeepaliveAck(pkt.payload, pkt.payloadLen);
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
				_enterBootloaderAt = nowMs() + 250u;
				break;

			default:
				// Unknown types and commands meant for the PC (PLAY_SONG, GET_SONG_LIST, ...) are not handled by the Base
				if (ack) sendNak(pkt, NakReason::UNKNOWN_MSG, via);
				break;
		}
	}

	// ── Routing ────────────────────────────────────────────────────────────

	static void forward(const Packet &pkt, Transport from) {
		Transport to = (from == Transport::USB_CDC) ? Transport::BLE : Transport::USB_CDC;

		// a nonce the remote has not heard yet goes first (the link may have become usable since the hello)
		if (to == Transport::BLE && _nonceRelay.pending(_pcSession.nonce())) sendKeepaliveToRemote(true);
		bool reachable = (to == Transport::BLE) ? Bluetooth::canSendData() : pcReachable();
		if (reachable && transmit(pkt, to)) {
			if (to == Transport::BLE) LED::notifyActivity(LED::PIXEL_BLUETOOTH);
			return;
		}

		// Cannot forward: the sender stops retrying (NAK is not sent for unacknowledged messages)
		if (!needsAck(pkt)) return;
		// BUSY only when the link is usable (connected, stream open and peer accepted = secured remote),
		// but the TX path is briefly occupied (module command mode). Connected but the stream not yet open, or
		// the link not yet secured (after %CONNECT%), is still UNREACHABLE for the PC: BUSY would force it to retry for as long as
		// the Remote is unavailable.
		bool linkUp = (to == Transport::BLE) ? bleLinkUsable() : pcReachable();
		sendNak(pkt, linkUp ? NakReason::BUSY : NakReason::UNREACHABLE, from);
	}

	static void processPacket(const Packet &pkt, Transport via) {
		duration now = stmcpp::systick::getDuration();
		Debug::log(Debug::Level::DEBUG, "router", "rx %s 0x%02x->0x%02x pid %u type 0x%02x len %u", via == Transport::USB_CDC ? "cdc" : "ble", pkt.source, pkt.dest,
				   pkt.packetId, static_cast<unsigned>(pkt.msgType), pkt.payloadLen);
		if (via == Transport::USB_CDC) {
			_pc.onRx(nowMs());
			_backstop.onPcPacket(nowMs());
		} else {
			_lastRemoteRx = now;
			_lastRemoteRxMs = nowMs();
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
				if (via != Transport::USB_CDC) break; // Remote to itself — drop
				observeSignal(pkt);                  // the Base does not consume it, only observes (OLED)
				_remoteFw.observe(pkt, nowMs());     // PC -> Remote: START/DATA/END
				forward(pkt, via);
				break;

			case protocol::ADDR_PC:
				if (via != Transport::BLE) break;
				_remoteFw.observe(pkt, nowMs());     // Remote -> PC: FW_UPDATE_STATUS
				forward(pkt, via);
				break;

			default:
				break; // Unknown address
		}
	}

	// Changes in PC/Remote availability and a summary of dropped frames (at most once per second) into the log
	static void logChanges() {
		// SYS-PC-03: PC present -> lost (USB unplugged/unmounted or keepalive timeout): stop the organ sounding
		// and switch the SSR off. Once per transition; a base that never saw a PC keeps power (D19).
		if (_pcLossGuard.update(tud_mounted() && isPcAlive())) {
			midi::sendStuckNoteCleanup();
			base::current::disable();
			GUI::syncPowerCheckbox();
			// SM-1: nothing plays any more; "Zastaveno", then the "Cekam na PC" overlay over the main menu
			GUI::dismissNowPlaying();
			// Q1-8 b: a restarted PC process numbers its packets from 1 again; do not ACK-and-drop its first ones
			_dedup.forget(protocol::ADDR_PC);
			_pcSession.forget();
			Debug::log(Debug::Level::WARNING, "router", "PC lost: all notes off, organ power off");
		}
		// Q1-1 c: backstop for a manual on without a PC (D19 keeps it on otherwise): All Notes Off, SSR off
		if (_backstop.poll(nowMs(), base::current::isEnabled())) {
			midi::sendStuckNoteCleanup();
			base::current::disable();
			GUI::syncPowerCheckbox();
			Debug::log(Debug::Level::WARNING, "router", "no PC packet for 2 h: all notes off, organ power off");
		}
		bool pc = isPcAlive();
		if (pc != _pcAliveLogged) {
			_pcAliveLogged = pc;
			Debug::log(pc ? Debug::Level::INFO : Debug::Level::WARNING, "router", "PC %s", pc ? "alive" : "lost");
		}
		bool remote = isRemoteAlive();
		if (remote != _remoteAliveLogged) {
			_remoteAliveLogged = remote;
			if (!remote) _remoteBoot.forget(); // never report a stale boot id (A2)
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

	// Remote update screen on the OLED: tick (stall, disappearance of the result) and redraw on change
	static void updateFwScreen() {
		_remoteFw.tick(nowMs());
		if (!_remoteFw.takeChange()) return;
		using State = boot::FwProgress::State;
		switch (_remoteFw.state()) {
			case State::Transfer: GUI::showFwProgress("Prenos do ovladace", _remoteFw.percent()); break;
			case State::Done: GUI::showFwProgress("Instalace ovladace", _remoteFw.percent()); break;
			case State::Failed: GUI::showFwProgress("Chyba aktualizace", _remoteFw.percent()); break;
			case State::Idle: GUI::hideFwProgress(); break;
		}
	}

	void process() {
		_pc.update(nowMs()); // every pass, so the PC-loss latch sees each age before the ms clock wraps
		// Receive from USB-CDC
		if (tud_cdc_available()) {
			uint8_t buf[64];
			uint32_t count = tud_cdc_read(buf, sizeof(buf));
			LED::notifyActivity(LED::PIXEL_USB);
			for (uint32_t i = 0; i < count; ++i) {
				if (_cdcParser.feed(buf[i])) {
					processPacket(_cdcParser.getPacket(), Transport::USB_CDC);
				} else if (buf[i] == protocol::FRAME_END && _badFrames != 0xFFFF) {
					_badFrames++; // FRAME_END without a valid frame: CRC, length or an incomplete frame
				}
			}
		}

		// Receive from BLE (packet queue filled by Bluetooth::process)
		Packet pkt;
		while (Bluetooth::getProtocolPacket(pkt)) {
			LED::notifyActivity(LED::PIXEL_BLUETOOTH);
			processPacket(pkt, Transport::BLE);
		}

		retryPending();
		sendKeepaliveToRemote();
		flushCdc();
		logChanges();
		updateFwScreen();

		// Pending reboot into the USB bootloader (after the ACK has been flushed).
		if (_enterBootloader && routercore::deadlineReached(nowMs(), _enterBootloaderAt)) {
			tud_cdc_write_flush();
			NVIC_SystemReset();
		}
	}

} // namespace router
