#ifndef ROUTER_HPP
#define ROUTER_HPP

#include <cstdint>
#include "protocol/types.hpp"

// Směrování protokolových paketů mezi PC (USB-CDC) a Remote (BLE) a lokální obsluha
// paketů pro Base. Pravidla viz lib/protocol/PROTOCOL.md (sekce Routing / Reliability).
namespace router {

	enum class Transport : uint8_t {
		USB_CDC,
		BLE,
	};

	// Hlavní smyčka: čtení CDC a BLE, opakování nepotvrzených paketů, keepalive k Remote, odesílání CDC
	void process();

	// Odešle paket, který Base sama vytváří, s potvrzením a opakováním (3 × 500 ms, stejné PacketId).
	// source a packetId doplní router. Vrací false, když je fronta plná nebo cíl nedostupný.
	bool sendReliable(protocol::Packet &pkt);

	bool isPcAlive();
	bool isRemoteAlive();

} // namespace router

#endif
