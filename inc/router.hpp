#ifndef ROUTER_HPP
#define ROUTER_HPP

#include <cstdint>
#include "protocol/types.hpp"

// Routing of protocol packets between the PC (USB-CDC) and the Remote (BLE) and local handling
// of packets for the Base. For the rules see lib/protocol/PROTOCOL.md (Routing / Reliability section).
namespace router {

	enum class Transport : uint8_t {
		USB_CDC,
		BLE,
	};

	// Main loop: reading CDC and BLE, retrying unacknowledged packets, keepalive to the Remote, CDC transmission
	void process();

	// Sends a packet that the Base itself creates, with acknowledgement and retries (3 × 500 ms, same PacketId).
	// source and packetId are filled in by the router. Returns false when the queue is full or the destination is unreachable.
	bool sendReliable(protocol::Packet &pkt);

	bool isPcAlive();
	bool isRemoteAlive();

	// Stale-link guard (BE-1): router clock (ms, wrapping uint32) and the time of the last frame received over BLE.
	// remoteRx returns false when no BLE frame was ever received since boot.
	uint32_t clockMs();
	bool remoteRx(uint32_t &atMs);
	// True while a remote firmware update is shown (transfer, install/result screen); the remote may be silent then.
	bool remoteFwBusy();

} // namespace router

#endif
