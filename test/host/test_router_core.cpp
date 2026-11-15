// Router logic (src/logic/router_core.*) as src/router.cpp drives it: time is the router clock,
// nowUs() = getTicks() * TICK_US (TICK_US = 1000, SysTick 1 ms), uint32 µs that wraps every 71.58 min.
// Ported from the step-2 verifier red tests (BF-1, PM-13, PM-6) onto the real code. DISABLED_ = known red, enabled by its fix.
#include <gtest/gtest.h>

#include "protocol/constants.hpp"
#include "protocol/types.hpp"
#include "recovery.hpp"
#include "router_core.hpp"

using protocol::MsgType;
using protocol::NakReason;
using protocol::Packet;

namespace {
	constexpr uint32_t TICK_US = 1000;
	constexpr uint32_t usAt(uint32_t ticks) { return ticks * TICK_US; } // router.cpp nowUs()

	Packet setCurrent(uint8_t pid, bool on) {
		Packet p{};
		p.source	 = protocol::ADDR_PC;
		p.dest		 = protocol::ADDR_BASE;
		p.packetId	 = pid;
		p.msgType	 = MsgType::SET_CURRENT;
		p.payload[0] = on ? 1 : 0;
		p.payloadLen = 1;
		return p;
	}

	// router.cpp handleLocal(): dedup, then SET_CURRENT drives the SSR
	struct Base {
		routercore::RxDedup dedup;
		bool				ssrOn = false;
		int					acks  = 0;
		void				handle(const Packet &pkt) {
			   if (dedup.isRepeat(pkt.source, pkt.packetId)) {
				   acks++;
				   return;
			   }
			   if (pkt.msgType == MsgType::SET_CURRENT) ssrOn = pkt.payload[0] != 0;
			   acks++;
		}
	};

	Packet reply(MsgType type, uint8_t pid, NakReason reason = NakReason::UNKNOWN_MSG) {
		Packet p{};
		p.source	 = protocol::ADDR_PC;
		p.dest		 = protocol::ADDR_BASE;
		p.msgType	 = type;
		p.payload[0] = pid;
		p.payload[1] = static_cast<uint8_t>(reason);
		p.payloadLen = type == MsgType::NAK ? 2 : 1;
		return p;
	}
} // namespace

// BF-1: the PC talks for 10 s, then goes silent while USB stays mounted; the organist plays by hand for 3 h.
// The loss must fire once and the PC must never look alive again until it sends a packet.
TEST(BF1, DISABLED_PcSilenceIsNeverSeenAsAliveAgain) {
	routercore::PeerPresence pc(protocol::KEEPALIVE_LOSS_MS * 1000u);
	recovery::PcLossGuard	 guard;
	int						 lossEvents		 = 0;
	uint32_t				 falseAliveMs	 = 0;
	uint32_t				 firstFalseAlive = 0;
	for (uint32_t t = 10; t <= 3u * 3600u * 1000u; t += 10) {
		if (t <= 10000 && t % 2000 == 0) pc.onRx(usAt(t)); // KEEPALIVE every 2 s
		pc.update(usAt(t));
		bool alive = pc.alive();
		if (guard.update(alive)) lossEvents++; // router.cpp logChanges() -> SSR off + All Notes Off
		if (t > 10000 + protocol::KEEPALIVE_LOSS_MS && alive) {
			if (!firstFalseAlive) firstFalseAlive = t;
			falseAliveMs += 10;
		}
	}
	EXPECT_EQ(lossEvents, 1) << "PcLossGuard fired again: SSR switched off under a manual player";
	EXPECT_EQ(falseAliveMs, 0u) << "silent PC reported alive again, first at t=" << firstFalseAlive << " ms";
}

// Control: a PC that keeps talking stays alive, and a packet after a loss makes it alive again.
TEST(BF1, PcAliveWhileTalkingAndAgainAfterAPacket) {
	routercore::PeerPresence pc(protocol::KEEPALIVE_LOSS_MS * 1000u);
	pc.update(usAt(100));
	EXPECT_FALSE(pc.alive()) << "never seen";
	for (uint32_t t = 1000; t <= 20000; t += 1000) {
		pc.onRx(usAt(t));
		pc.update(usAt(t + 500));
		EXPECT_TRUE(pc.alive()) << "t=" << t;
	}
	pc.update(usAt(20000 + protocol::KEEPALIVE_LOSS_MS));
	EXPECT_FALSE(pc.alive());
	pc.onRx(usAt(40000));
	pc.update(usAt(40010));
	EXPECT_TRUE(pc.alive());
}

// nowMs() feeds boot::FwProgress, which handles uint32 ms wraparound; it must not wrap at 2^32 µs.
TEST(BF1, DISABLED_NowMsIsMonotonicPast71Minutes) {
	uint32_t before = routercore::msFromTicks(4294967u, TICK_US);
	uint32_t after	= routercore::msFromTicks(4294968u, TICK_US);
	EXPECT_GT(after, before) << "nowMs() jumped from " << before << " to " << after;
}

// The bootloader-entry deadline must be reached even when it lies past the clock wrap.
TEST(BF1, DISABLED_DeadlineAcrossClockWrap) {
	uint32_t now	  = 0xFFFFFF00u;
	uint32_t deadline = now + 0x200u; // wraps
	EXPECT_FALSE(routercore::deadlineReached(now, deadline));
	EXPECT_TRUE(routercore::deadlineReached(deadline, deadline));
	EXPECT_TRUE(routercore::deadlineReached(deadline + 5u, deadline));
}

// PM-13 (base side): organOn pid 5, ACK lost; organOff pid 6; the PC retries pid 5.
TEST(PM13, DISABLED_StaleRetryOfOnAfterOffMustNotPowerTheOrgan) {
	Base b;
	b.handle(setCurrent(5, true));
	b.handle(setCurrent(6, false));
	ASSERT_FALSE(b.ssrOn);
	b.handle(setCurrent(5, true)); // retransmission of an already executed, superseded command
	EXPECT_FALSE(b.ssrOn) << "stale SET_CURRENT(on) retry executed after SET_CURRENT(off): SSR on with nobody playing";
}

// Learning 2026-11-07: a new PC process restarts its PacketId counter.
TEST(PM13, DISABLED_NewPcSessionCommandIsNotSwallowedAsDuplicate) {
	Base b;
	b.handle(setCurrent(1, true)); // old process, last frame
	b.handle(setCurrent(1, false)); // new process, first reliable frame, pid 1 again
	EXPECT_FALSE(b.ssrOn) << "SET_CURRENT(off) of a new PC session ACKed but not executed";
}

// Control: an immediate retransmission is ACKed and not executed twice.
TEST(PM13, ImmediateDuplicateSuppressed) {
	Base b;
	b.handle(setCurrent(20, true));
	b.ssrOn = false; // if the duplicate executes, it turns the SSR back on
	b.handle(setCurrent(20, true));
	EXPECT_FALSE(b.ssrOn);
	EXPECT_EQ(b.acks, 2);
}

// PM-6: PROTOCOL.md says the sender retries on NAK BUSY.
TEST(PM6, DISABLED_BaseRetriesAfterNakBusy) {
	routercore::RetryQueue q;
	Packet				   stop{};
	stop.source	  = protocol::ADDR_BASE;
	stop.dest	  = protocol::ADDR_PC;
	stop.msgType  = MsgType::STOP_SONG;
	stop.packetId = 7;
	int transmits = 1;
	ASSERT_TRUE(q.add(stop, usAt(0)));
	q.resolve(reply(MsgType::NAK, 7, NakReason::BUSY));
	q.poll(usAt(protocol::RETRY_TIMEOUT_MS), protocol::RETRY_TIMEOUT_MS * 1000u, protocol::MAX_RETRIES, [&](const Packet &) { transmits++; },
		   [](const Packet &) {});
	EXPECT_EQ(transmits, 2) << "NAK BUSY treated as final: STOP_SONG never retried";
}

// Control: no reply -> one retransmission per RETRY_TIMEOUT, then give up after MAX_RETRIES; an ACK closes the entry.
TEST(PM6, RetransmitsWithoutReplyThenGivesUp) {
	routercore::RetryQueue q;
	Packet				   stop{};
	stop.dest	  = protocol::ADDR_PC;
	stop.msgType  = MsgType::STOP_SONG;
	stop.packetId = 3;
	ASSERT_TRUE(q.add(stop, usAt(0)));
	int		 resends = 0, gaveUp = 0;
	uint32_t t		 = 0;
	for (int i = 0; i <= protocol::MAX_RETRIES; i++) {
		t += protocol::RETRY_TIMEOUT_MS;
		q.poll(usAt(t), protocol::RETRY_TIMEOUT_MS * 1000u, protocol::MAX_RETRIES, [&](const Packet &p) { resends++; EXPECT_EQ(p.packetId, 3); },
			   [&](const Packet &) { gaveUp++; });
	}
	EXPECT_EQ(resends, protocol::MAX_RETRIES);
	EXPECT_EQ(gaveUp, 1);

	ASSERT_TRUE(q.add(stop, usAt(t)));
	q.resolve(reply(MsgType::ACK, 3));
	q.poll(usAt(t + protocol::RETRY_TIMEOUT_MS), protocol::RETRY_TIMEOUT_MS * 1000u, protocol::MAX_RETRIES, [&](const Packet &) { resends++; },
		   [&](const Packet &) { gaveUp++; });
	EXPECT_EQ(resends, protocol::MAX_RETRIES);
	EXPECT_EQ(gaveUp, 1);
}
