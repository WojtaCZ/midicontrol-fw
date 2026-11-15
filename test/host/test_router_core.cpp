// Router logic (src/logic/router_core.*) as src/router.cpp drives it: time is the router clock,
// nowMs() = msFromTicks(getTicks(), TICK_US) (TICK_US = 1000, SysTick 1 ms), uint32 ms that wraps every 49.7 days.
// Ported from the step-2 verifier red tests (BF-1, PM-13, PM-6) onto the real code. DISABLED_ = known red, enabled by its fix.
#include <gtest/gtest.h>

#include <vector>

#include "protocol/constants.hpp"
#include "protocol/types.hpp"
#include "recovery.hpp"
#include "router_core.hpp"

using protocol::MsgType;
using protocol::NakReason;
using protocol::Packet;

namespace {
	constexpr uint32_t TICK_US = 1000;
	uint32_t		   msAt(uint32_t ticks) { return routercore::msFromTicks(ticks, TICK_US); } // router.cpp nowMs()

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
// `start` = SysTick count at t = 0; the ticks (and the ms clock) may wrap during the run.
static void pcSilentFor3Hours(uint32_t start) {
	routercore::PeerPresence pc(protocol::KEEPALIVE_LOSS_MS);
	recovery::PcLossGuard	 guard;
	int						 lossEvents		 = 0;
	uint32_t				 falseAliveMs	 = 0;
	uint32_t				 firstFalseAlive = 0;
	for (uint32_t t = 10; t <= 3u * 3600u * 1000u; t += 10) {
		if (t <= 10000 && t % 2000 == 0) pc.onRx(msAt(start + t)); // KEEPALIVE every 2 s
		pc.update(msAt(start + t));
		bool alive = pc.alive();
		if (t <= 10000) EXPECT_TRUE(t < 2000 || alive) << "t=" << t;
		if (guard.update(alive)) lossEvents++; // router.cpp logChanges() -> SSR off + All Notes Off
		if (t > 10000 + protocol::KEEPALIVE_LOSS_MS && alive) {
			if (!firstFalseAlive) firstFalseAlive = t;
			falseAliveMs += 10;
		}
	}
	EXPECT_EQ(lossEvents, 1) << "PcLossGuard fired again: SSR switched off under a manual player";
	EXPECT_EQ(falseAliveMs, 0u) << "silent PC reported alive again, first at t=" << firstFalseAlive << " ms";
}

TEST(BF1, PcSilenceIsNeverSeenAsAliveAgain) {
	pcSilentFor3Hours(0);
}

// The same with the uint32 ms clock wrapping (49.7 days of uptime) 5 s into the silence and right at the loss.
TEST(BF1, PcSilenceAcrossMsClockWrap) {
	pcSilentFor3Hours(0xFFFFFFFFu - 15000u);
	pcSilentFor3Hours(0xFFFFFFFFu - 16000u - protocol::KEEPALIVE_LOSS_MS);
}

// A talking PC stays alive across the ms clock wrap.
TEST(BF1, PcAliveAcrossMsClockWrap) {
	routercore::PeerPresence pc(protocol::KEEPALIVE_LOSS_MS);
	uint32_t				 start = 0xFFFFFFFFu - 30000u;
	for (uint32_t t = 0; t <= 60000; t += 10) {
		if (t % 2000 == 0) pc.onRx(msAt(start + t));
		pc.update(msAt(start + t));
		ASSERT_TRUE(pc.alive()) << "t=" << t;
	}
}

// Control: a PC that keeps talking stays alive, and a packet after a loss makes it alive again.
TEST(BF1, PcAliveWhileTalkingAndAgainAfterAPacket) {
	routercore::PeerPresence pc(protocol::KEEPALIVE_LOSS_MS);
	pc.update(msAt(100));
	EXPECT_FALSE(pc.alive()) << "never seen";
	for (uint32_t t = 1000; t <= 20000; t += 1000) {
		pc.onRx(msAt(t));
		pc.update(msAt(t + 500));
		EXPECT_TRUE(pc.alive()) << "t=" << t;
	}
	pc.update(msAt(20000 + protocol::KEEPALIVE_LOSS_MS));
	EXPECT_FALSE(pc.alive());
	pc.onRx(msAt(40000));
	pc.update(msAt(40010));
	EXPECT_TRUE(pc.alive());
}

// nowMs() feeds boot::FwProgress, which handles uint32 ms wraparound; it must not wrap at 2^32 µs.
TEST(BF1, NowMsIsMonotonicPast71Minutes) {
	uint32_t before = routercore::msFromTicks(4294967u, TICK_US);
	uint32_t after	= routercore::msFromTicks(4294968u, TICK_US);
	EXPECT_GT(after, before) << "nowMs() jumped from " << before << " to " << after;
}

// The bootloader-entry deadline must be reached even when it lies past the clock wrap.
TEST(BF1, DeadlineAcrossClockWrap) {
	uint32_t now	  = 0xFFFFFF00u;
	uint32_t deadline = now + 0x200u; // wraps
	EXPECT_FALSE(routercore::deadlineReached(now, deadline));
	EXPECT_TRUE(routercore::deadlineReached(deadline, deadline));
	EXPECT_TRUE(routercore::deadlineReached(deadline + 5u, deadline));
}

// PM-13 (base side): organOn pid 5, ACK lost; organOff pid 6; the PC retries pid 5.
// RESIDUAL, stays red on the base (no monotonic pid check by decision): covered on the PC side.
TEST(PM13, DISABLED_StaleRetryOfOnAfterOffMustNotPowerTheOrgan) {
	Base b;
	b.handle(setCurrent(5, true));
	b.handle(setCurrent(6, false));
	ASSERT_FALSE(b.ssrOn);
	b.handle(setCurrent(5, true)); // retransmission of an already executed, superseded command
	EXPECT_FALSE(b.ssrOn) << "stale SET_CURRENT(on) retry executed after SET_CURRENT(off): SSR on with nobody playing";
}

// Learning 2026-11-07: a new PC process restarts its PacketId counter.
// RESIDUAL when the base never declared the PC lost in between (restart faster than KEEPALIVE_LOSS):
// covered on the PC side. With a loss in between see NewPcSessionAfterPcLossIsExecuted.
TEST(PM13, DISABLED_NewPcSessionCommandIsNotSwallowedAsDuplicate) {
	Base b;
	b.handle(setCurrent(1, true)); // old process, last frame
	b.handle(setCurrent(1, false)); // new process, first reliable frame, pid 1 again
	EXPECT_FALSE(b.ssrOn) << "SET_CURRENT(off) of a new PC session ACKed but not executed";
}

// Q1-8 b: a PC restart that the base noticed (PC declared lost in between, router.cpp logChanges()) starts
// a fresh duplicate filter for the PC, so the new process's first pid is executed even if it repeats the old one.
TEST(PM13, NewPcSessionAfterPcLossIsExecuted) {
	Base b;
	b.handle(setCurrent(1, true)); // old process, last frame
	b.dedup.forget(protocol::ADDR_PC); // PC declared lost
	b.handle(setCurrent(1, false)); // new process, first reliable frame, pid 1 again
	EXPECT_FALSE(b.ssrOn) << "SET_CURRENT(off) of a new PC session ACKed but not executed";
}

// Forgetting the PC leaves the other nodes' filters alone.
TEST(PM13, ForgetPcKeepsRemoteFilter) {
	routercore::RxDedup d;
	EXPECT_FALSE(d.isRepeat(protocol::ADDR_REMOTE, 9));
	EXPECT_FALSE(d.isRepeat(protocol::ADDR_PC, 9));
	d.forget(protocol::ADDR_PC);
	EXPECT_TRUE(d.isRepeat(protocol::ADDR_REMOTE, 9));
	EXPECT_FALSE(d.isRepeat(protocol::ADDR_PC, 9));
	EXPECT_TRUE(d.isRepeat(protocol::ADDR_PC, 9));
	d.forget(200); // out of range: ignored
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
	ASSERT_TRUE(q.add(stop, msAt(0)));
	q.resolve(reply(MsgType::NAK, 7, NakReason::BUSY));
	q.poll(msAt(protocol::RETRY_TIMEOUT_MS), protocol::RETRY_TIMEOUT_MS, protocol::MAX_RETRIES, [&](const Packet &) { transmits++; },
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
	ASSERT_TRUE(q.add(stop, msAt(0)));
	int		 resends = 0, gaveUp = 0;
	uint32_t t		 = 0;
	for (int i = 0; i <= protocol::MAX_RETRIES; i++) {
		t += protocol::RETRY_TIMEOUT_MS;
		q.poll(msAt(t), protocol::RETRY_TIMEOUT_MS, protocol::MAX_RETRIES, [&](const Packet &p) { resends++; EXPECT_EQ(p.packetId, 3); },
			   [&](const Packet &) { gaveUp++; });
	}
	EXPECT_EQ(resends, protocol::MAX_RETRIES);
	EXPECT_EQ(gaveUp, 1);

	ASSERT_TRUE(q.add(stop, msAt(t)));
	q.resolve(reply(MsgType::ACK, 3));
	q.poll(msAt(t + protocol::RETRY_TIMEOUT_MS), protocol::RETRY_TIMEOUT_MS, protocol::MAX_RETRIES, [&](const Packet &) { resends++; },
		   [&](const Packet &) { gaveUp++; });
	EXPECT_EQ(resends, protocol::MAX_RETRIES);
	EXPECT_EQ(gaveUp, 1);
}

// Q1-1 c: 2 h backstop. Literal reading (coordinator): SSR on and no valid PC packet for 2 h -> SSR off +
// All Notes Off; a PC packet restarts the 2 h. Our choice (owner question): the SSR going off -> on also restarts it.
namespace {
	constexpr uint32_t TWO_H = 2u * 60u * 60u * 1000u;

	// Polls every second from start to start + span (router.cpp polls every pass); returns the fire times.
	std::vector<uint32_t> pollEverySecond(routercore::SsrBackstop &b, uint32_t start, uint32_t span, bool &ssr) {
		std::vector<uint32_t> fired;
		for (uint32_t t = 0; t <= span; t += 1000) {
			uint32_t now = start + t;
			if (b.poll(now, ssr)) {
				fired.push_back(t);
				ssr = false; // router: base::current::disable()
			}
		}
		return fired;
	}
} // namespace

TEST(Backstop, SsrOnWithoutPcGoesOffAfterTwoHours) {
	routercore::SsrBackstop b;
	bool ssr = true; // manual on, no PC ever seen
	auto fired = pollEverySecond(b, 1000, TWO_H + 60000, ssr);
	ASSERT_EQ(fired.size(), 1u) << "manual on without a PC must be cut once after 2 h";
	EXPECT_EQ(fired[0], TWO_H);
	EXPECT_FALSE(ssr);
}

TEST(Backstop, NotBeforeTwoHours) {
	routercore::SsrBackstop b;
	bool ssr = true;
	EXPECT_TRUE(pollEverySecond(b, 5000, TWO_H - 1000, ssr).empty());
	EXPECT_TRUE(ssr);
}

TEST(Backstop, PcPacketRestartsTheTwoHours) {
	routercore::SsrBackstop b;
	bool ssr = true;
	const uint32_t start = 1000;
	EXPECT_TRUE(pollEverySecond(b, start, TWO_H - 1000, ssr).empty());
	b.onPcPacket(start + TWO_H - 1000); // one packet just before the limit
	EXPECT_TRUE(pollEverySecond(b, start + TWO_H, TWO_H - 2000, ssr).empty()) << "PC packet must restart the 2 h";
	auto fired = pollEverySecond(b, start + 2 * TWO_H - 1000, 2000, ssr);
	ASSERT_EQ(fired.size(), 1u);
	EXPECT_EQ(fired[0], 0u); // exactly 2 h after the packet
}

TEST(Backstop, SsrOffNeverFires) {
	routercore::SsrBackstop b;
	bool ssr = false;
	EXPECT_TRUE(pollEverySecond(b, 0, 3 * TWO_H, ssr).empty());
}

TEST(Backstop, SwitchingOnRestartsTheTwoHours) {
	routercore::SsrBackstop b;
	bool ssr = false;
	EXPECT_TRUE(pollEverySecond(b, 0, 3 * TWO_H, ssr).empty()); // long off, no PC
	ssr = true; // switched on by hand at 3 h
	EXPECT_TRUE(pollEverySecond(b, 3 * TWO_H + 1000, TWO_H - 1000, ssr).empty()) << "off->on must restart the 2 h";
	auto fired = pollEverySecond(b, 4 * TWO_H + 1000, 1000, ssr);
	ASSERT_EQ(fired.size(), 1u);
}

TEST(Backstop, OnAgainAfterTheCutGetsAnotherTwoHours) {
	routercore::SsrBackstop b;
	bool ssr = true;
	ASSERT_EQ(pollEverySecond(b, 0, TWO_H, ssr).size(), 1u);
	ssr = true; // organist switches it on again from the base menu
	EXPECT_TRUE(pollEverySecond(b, TWO_H + 1000, TWO_H - 1000, ssr).empty());
	EXPECT_EQ(pollEverySecond(b, 2 * TWO_H + 1000, 1000, ssr).size(), 1u);
}

TEST(Backstop, AcrossMsClockWrap) {
	routercore::SsrBackstop b;
	bool ssr = true;
	const uint32_t start = msAt(0xFFFFFFFFu - TWO_H / 2); // ms clock wraps at 1 h
	auto fired = pollEverySecond(b, start, TWO_H + 60000, ssr);
	ASSERT_EQ(fired.size(), 1u) << "must fire exactly once across the wrap";
	EXPECT_EQ(fired[0], TWO_H);
}

TEST(Backstop, PcPacketAcrossMsClockWrap) {
	routercore::SsrBackstop b;
	bool ssr = true;
	const uint32_t start = 0xFFFFFFFFu - 10 * 60000u; // 10 min before the wrap
	EXPECT_TRUE(pollEverySecond(b, start, 60 * 60000u, ssr).empty());
	b.onPcPacket(start + 60 * 60000u); // after the wrap
	EXPECT_TRUE(pollEverySecond(b, start + 60 * 60000u, TWO_H - 1000, ssr).empty());
	EXPECT_EQ(pollEverySecond(b, start + 60 * 60000u + TWO_H, 1000, ssr).size(), 1u);
}
