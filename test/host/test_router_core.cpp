// Router logic (src/logic/router_core.*) as src/router.cpp drives it: time is the router clock,
// nowMs() = msFromTicks(getTicks(), TICK_US) (TICK_US = 1000, SysTick 1 ms), uint32 ms that wraps every 49.7 days.
// Ported from the step-2 verifier red tests (BF-1, PM-13, PM-6) onto the real code.
#include <gtest/gtest.h>

#include <cstring>
#include <string>
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
		routercore::RxDedup	   dedup;
		routercore::PcSession  session;
		bool				   ssrOn = false;
		int					acks  = 0;
		// router.cpp handleLocal() KEEPALIVE from the PC: the shared helper, payload [nonce lo, hi] (empty = v1 PC)
		void keepalive(uint16_t nonce) {
			const uint8_t pl[2] = {static_cast<uint8_t>(nonce), static_cast<uint8_t>(nonce >> 8)};
			routercore::onPcKeepalive(session, dedup, pl, 2);
		}
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
// Fixed by the duplicate window (last 8 PacketIds per source), not by a monotonic pid check.
TEST(PM13, StaleRetryOfOnAfterOffMustNotPowerTheOrgan) {
	Base b;
	b.handle(setCurrent(5, true));
	b.handle(setCurrent(6, false));
	ASSERT_FALSE(b.ssrOn);
	b.handle(setCurrent(5, true)); // retransmission of an already executed, superseded command
	EXPECT_FALSE(b.ssrOn) << "stale SET_CURRENT(on) retry executed after SET_CURRENT(off): SSR on with nobody playing";
}

// Learning 2026-11-07: a new PC process restarts its PacketId counter. Its hello KEEPALIVE carries a new session
// nonce, which makes the base forget the old process's PacketIds (also when the base never declared the PC lost).
TEST(PM13, NewPcSessionCommandIsNotSwallowedAsDuplicate) {
	Base b;
	b.keepalive(0x1111);
	b.handle(setCurrent(1, true)); // old process, last frame
	b.keepalive(0x2222); // new process: hello
	b.handle(setCurrent(1, false)); // new process, first reliable frame, pid 1 again
	EXPECT_FALSE(b.ssrOn) << "SET_CURRENT(off) of a new PC session ACKed but not executed";
}

TEST(PM13, NonceChangeForgetsPcDedup) {
	routercore::RxDedup	  d;
	routercore::PcSession s;
	const uint8_t		  n1[2] = {0x34, 0x12};
	const uint8_t		  n2[2] = {0x35, 0x12};
	routercore::onPcKeepalive(s, d, n1, 2);
	EXPECT_FALSE(d.isRepeat(protocol::ADDR_PC, 3));
	EXPECT_TRUE(d.isRepeat(protocol::ADDR_PC, 3));
	EXPECT_FALSE(d.isRepeat(protocol::ADDR_REMOTE, 3));
	routercore::onPcKeepalive(s, d, n2, 2);
	EXPECT_FALSE(d.isRepeat(protocol::ADDR_PC, 3)) << "new session: pid 3 is new again";
	EXPECT_TRUE(d.isRepeat(protocol::ADDR_REMOTE, 3)) << "only the PC's window is forgotten";
}

TEST(PM13, SameNonceKeepsDedup) {
	Base b;
	b.keepalive(0xBEEF);
	b.handle(setCurrent(20, true));
	b.ssrOn = false;
	b.keepalive(0xBEEF); // the periodic keepalive of the same process
	b.handle(setCurrent(20, true));
	EXPECT_FALSE(b.ssrOn) << "retry executed after a keepalive with the same nonce";
}

TEST(PM13, ZeroNonceNeverResets) {
	Base b;
	b.handle(setCurrent(7, true));
	b.ssrOn = false;
	b.keepalive(0); // v1 PC: nonce 0
	routercore::PcSession s;
	EXPECT_FALSE(s.observe(0));
	routercore::RxDedup d;
	routercore::onPcKeepalive(s, d, nullptr, 0); // empty payload (old PC)
	b.handle(setCurrent(7, true));
	EXPECT_FALSE(b.ssrOn) << "nonce 0 reset the duplicate window";
	b.keepalive(0x1234);
	b.keepalive(0); // a zero after a real nonce does not count as a change either
	b.handle(setCurrent(8, true));
	b.ssrOn = false;
	b.handle(setCurrent(8, true));
	EXPECT_FALSE(b.ssrOn);
}

TEST(PM13, NonceForgottenOnPcLossSoSameNonceRestartWorks) {
	routercore::PcSession s;
	EXPECT_TRUE(s.observe(0x4242));
	EXPECT_FALSE(s.observe(0x4242));
	s.forget(); // PC declared lost (router.cpp logChanges())
	EXPECT_TRUE(s.observe(0x4242)) << "restarted PC with the same nonce after a loss is a new session";
}

// router.cpp order: a PC hello with a new nonce is relayed to the remote at once, before a later reliable frame of the
// same loop pass; when the BLE link cannot send yet, the nonce stays pending and still goes out first.
namespace {
	struct RelaySim {
		routercore::NonceRelay relay;
		routercore::PcSession  session;
		routercore::RxDedup	   dedup;
		bool				   bleUp = true;
		std::vector<std::string> remoteTx; // what the remote hears, in order
		void sendKeepalive() {
			if (!bleUp) return;
			relay.sent(session.nonce());
			remoteTx.push_back("KA" + std::to_string(session.nonce()));
		}
		void pcHello(uint16_t nonce) {
			const uint8_t pl[2] = {static_cast<uint8_t>(nonce), static_cast<uint8_t>(nonce >> 8)};
			if (routercore::onPcKeepalive(session, dedup, pl, 2)) sendKeepalive();
		}
		void forwardToRemote(const char *what) {
			if (relay.pending(session.nonce())) sendKeepalive();
			if (bleUp) remoteTx.push_back(what);
		}
	};
} // namespace

TEST(PM13, NonceKeepaliveReachesRemoteBeforeLaterFrameOfSamePass) {
	RelaySim r;
	r.pcHello(0x1111);
	r.remoteTx.clear();
	r.pcHello(0x2222);
	r.forwardToRemote("CMD");
	ASSERT_EQ(r.remoteTx.size(), 2u);
	EXPECT_EQ(r.remoteTx[0], "KA" + std::to_string(0x2222));
	EXPECT_EQ(r.remoteTx[1], "CMD");
}

TEST(PM13, NonceKeepaliveStaysPendingWhileLinkDownThenGoesFirst) {
	RelaySim r;
	r.bleUp = false;
	r.pcHello(0x3333);
	r.forwardToRemote("CMD");
	EXPECT_TRUE(r.remoteTx.empty());
	EXPECT_TRUE(r.relay.pending(0x3333));
	r.bleUp = true;
	r.forwardToRemote("CMD2");
	ASSERT_EQ(r.remoteTx.size(), 2u);
	EXPECT_EQ(r.remoteTx[0], "KA" + std::to_string(0x3333));
	EXPECT_EQ(r.remoteTx[1], "CMD2");
	EXPECT_FALSE(r.relay.pending(0x3333));
}

TEST(PM13, WindowSuppressesOlderRetry) {
	routercore::RxDedup d;
	for (uint8_t id = 1; id <= 8; ++id) EXPECT_FALSE(d.isRepeat(protocol::ADDR_PC, id));
	for (uint8_t id = 1; id <= 8; ++id) EXPECT_TRUE(d.isRepeat(protocol::ADDR_PC, id)) << int(id);
	EXPECT_FALSE(d.isRepeat(protocol::ADDR_PC, 9)); // pushes 1 out of the window
	EXPECT_TRUE(d.isRepeat(protocol::ADDR_PC, 2));
	EXPECT_TRUE(d.isRepeat(protocol::ADDR_PC, 9));
}

TEST(PM13, WindowDoesNotHideNewIdAfterWrap) {
	routercore::RxDedup d;
	EXPECT_FALSE(d.isRepeat(protocol::ADDR_PC, 1));
	for (uint8_t id = 2; id <= 9; ++id) EXPECT_FALSE(d.isRepeat(protocol::ADDR_PC, id));
	// ids 1 is now older than 8 newer ids: the PacketId counter wrapped, 1 is a new command
	EXPECT_FALSE(d.isRepeat(protocol::ADDR_PC, 1));
	EXPECT_TRUE(d.isRepeat(protocol::ADDR_PC, 1));
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
TEST(PM6, BaseRetriesAfterNakBusy) {
	routercore::RetryQueue q;
	Packet				   stop{};
	stop.source	  = protocol::ADDR_BASE;
	stop.dest	  = protocol::ADDR_PC;
	stop.msgType  = MsgType::STOP_SONG;
	stop.packetId = 7;
	int transmits = 1;
	ASSERT_TRUE(q.add(stop, msAt(0)));
	EXPECT_FALSE(q.resolve(reply(MsgType::NAK, 7, NakReason::BUSY))) << "BUSY = not now: the entry stays";
	q.poll(msAt(protocol::RETRY_TIMEOUT_MS), protocol::RETRY_TIMEOUT_MS, protocol::MAX_RETRIES, [&](const Packet &) { transmits++; },
		   [](const Packet &, bool) {});
	EXPECT_EQ(transmits, 2) << "NAK BUSY treated as final: STOP_SONG never retried";
}

// PM-6: BUSY on every try: bounded by MAX_RETRIES like silence, and the give-up says BUSY (PC zaneprazdneno).
TEST(PM6, BusyEveryTimeGivesUpAsBusy) {
	routercore::RetryQueue q;
	Packet				   play{};
	play.dest	  = protocol::ADDR_PC;
	play.msgType  = MsgType::PLAY_SONG;
	play.packetId = 11;
	ASSERT_TRUE(q.add(play, msAt(0)));
	int		 resends = 0, gaveUp = 0;
	bool	 lastBusy = false;
	uint32_t t		  = 0;
	for (int i = 0; i <= protocol::MAX_RETRIES; i++) {
		q.resolve(reply(MsgType::NAK, 11, NakReason::BUSY));
		t += protocol::RETRY_TIMEOUT_MS;
		q.poll(msAt(t), protocol::RETRY_TIMEOUT_MS, protocol::MAX_RETRIES, [&](const Packet &) { resends++; },
			   [&](const Packet &, bool busy) { gaveUp++; lastBusy = busy; });
	}
	EXPECT_EQ(resends, protocol::MAX_RETRIES);
	ASSERT_EQ(gaveUp, 1);
	EXPECT_TRUE(lastBusy);
	EXPECT_FALSE(q.full());
}

// PM-6: BUSY then ACK closes normally; other NAK reasons stay final (PM-1).
TEST(PM6, BusyThenAckAndOtherNaksFinal) {
	routercore::RetryQueue q;
	Packet				   p{};
	p.dest	   = protocol::ADDR_PC;
	p.msgType  = MsgType::PLAY_SONG;
	p.packetId = 12;
	ASSERT_TRUE(q.add(p, msAt(0)));
	EXPECT_FALSE(q.resolve(reply(MsgType::NAK, 12, NakReason::BUSY)));
	routercore::RetryQueue::Closed c{};
	EXPECT_TRUE(q.resolve(reply(MsgType::ACK, 12), &c));
	EXPECT_FALSE(c.nak);
	p.packetId = 13;
	ASSERT_TRUE(q.add(p, msAt(0)));
	EXPECT_TRUE(q.resolve(reply(MsgType::NAK, 13, NakReason::NOT_FOUND), &c));
	EXPECT_EQ(c.reason, NakReason::NOT_FOUND);
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
			   [&](const Packet &, bool busy) { gaveUp++; EXPECT_FALSE(busy); });
	}
	EXPECT_EQ(resends, protocol::MAX_RETRIES);
	EXPECT_EQ(gaveUp, 1);

	ASSERT_TRUE(q.add(stop, msAt(t)));
	q.resolve(reply(MsgType::ACK, 3));
	q.poll(msAt(t + protocol::RETRY_TIMEOUT_MS), protocol::RETRY_TIMEOUT_MS, protocol::MAX_RETRIES, [&](const Packet &) { resends++; },
		   [&](const Packet &, bool) { gaveUp++; });
	EXPECT_EQ(resends, protocol::MAX_RETRIES);
	EXPECT_EQ(gaveUp, 1);
}

// PM-1/UI2-1: the base's own requests (PLAY_SONG, STOP_SONG, GET_SONG_LIST) learn why they failed.
namespace {
	Packet basePacket(MsgType type, uint8_t pid) {
		Packet p{};
		p.source   = protocol::ADDR_BASE;
		p.dest	   = protocol::ADDR_PC;
		p.msgType  = type;
		p.packetId = pid;
		return p;
	}
} // namespace

TEST(PM1, NakReasonReachesTheOriginator) {
	routercore::RetryQueue q;
	ASSERT_TRUE(q.add(basePacket(MsgType::PLAY_SONG, 9), msAt(0)));
	routercore::RetryQueue::Closed c{};
	ASSERT_TRUE(q.resolve(reply(MsgType::NAK, 9, NakReason::NOT_FOUND), &c)) << "NAK must close the entry and say so";
	EXPECT_EQ(c.type, MsgType::PLAY_SONG);
	EXPECT_TRUE(c.nak);
	EXPECT_EQ(c.reason, NakReason::NOT_FOUND);
	EXPECT_FALSE(q.resolve(reply(MsgType::NAK, 9, NakReason::NOT_FOUND), &c)) << "already closed";
}

TEST(PM1, AckIsNotAFailure) {
	routercore::RetryQueue q;
	ASSERT_TRUE(q.add(basePacket(MsgType::GET_SONG_LIST, 4), msAt(0)));
	routercore::RetryQueue::Closed c{};
	ASSERT_TRUE(q.resolve(reply(MsgType::ACK, 4), &c));
	EXPECT_EQ(c.type, MsgType::GET_SONG_LIST);
	EXPECT_FALSE(c.nak);
}

TEST(PM1, ReplyForAnotherPidClosesNothing) {
	routercore::RetryQueue q;
	ASSERT_TRUE(q.add(basePacket(MsgType::STOP_SONG, 5), msAt(0)));
	EXPECT_FALSE(q.resolve(reply(MsgType::ACK, 6)));
	EXPECT_FALSE(q.full());
}

TEST(PM1, FailureTexts) {
	using routercore::Failure;
	using routercore::failureText;
	ASSERT_NE(failureText(Failure::Nak, NakReason::NOT_FOUND), nullptr);
	EXPECT_STREQ(failureText(Failure::Nak, NakReason::NOT_FOUND), "Pisen nenalezena");
	EXPECT_STREQ(failureText(Failure::Nak, NakReason::BUSY), "PC zaneprazdneno");
	EXPECT_STREQ(failureText(Failure::Nak, NakReason::UNREACHABLE), "PC nedostupne");
	EXPECT_STREQ(failureText(Failure::Nak, NakReason::NOT_PLAYING), "Nehraje se");
	EXPECT_STREQ(failureText(Failure::Nak, NakReason::INVALID_PAYLOAD), "Odmitnuto");
	EXPECT_STREQ(failureText(Failure::Nak, NakReason::UNKNOWN_MSG), "Odmitnuto");
	EXPECT_STREQ(failureText(Failure::NoAnswer), "PC neodpovida");
	EXPECT_STREQ(failureText(Failure::NotSent), "Neodeslano");
	// one 7x10 line on the 128 px OLED, plain ASCII (no diacritics)
	for (auto f : {Failure::Nak, Failure::NoAnswer, Failure::NotSent})
		for (uint8_t r = 1; r <= 7; r++) {
			const char *t = failureText(f, static_cast<NakReason>(r));
			ASSERT_NE(t, nullptr);
			EXPECT_LE(std::strlen(t), 18u) << t;
			for (const char *c = t; *c; c++) EXPECT_TRUE(*c >= 0x20 && *c < 0x7F) << t;
		}
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

// A2: remote boot id from KEEPALIVE_ACK, reported in STATUS_RESP (protocol v2, 7 B)
TEST(RemoteBoot, AckWithThreeBytesStoresId) {
	routercore::RemoteBoot b;
	const uint8_t		   ack[3] = {0xD8, 0x34, 0x12}; // rssi, id lo, id hi
	b.onKeepaliveAck(ack, 3);
	EXPECT_TRUE(b.valid());
	EXPECT_EQ(b.id(), 0x1234);
}

TEST(RemoteBoot, AckWithOneByteKeepsInvalid) {
	routercore::RemoteBoot b;
	const uint8_t		   ack[1] = {0xD8};
	b.onKeepaliveAck(ack, 1);
	EXPECT_FALSE(b.valid());
	b.onKeepaliveAck(ack, 0);
	EXPECT_FALSE(b.valid());
}

TEST(RemoteBoot, ShortAckDoesNotDisturbStoredId) {
	routercore::RemoteBoot b;
	const uint8_t		   ack3[3] = {0, 0x01, 0x02};
	const uint8_t		   ack1[1] = {0};
	b.onKeepaliveAck(ack3, 3);
	b.onKeepaliveAck(ack1, 1);
	EXPECT_TRUE(b.valid());
	EXPECT_EQ(b.id(), 0x0201);
}

TEST(RemoteBoot, NewIdReplacesOld) {
	routercore::RemoteBoot b;
	const uint8_t		   a[3] = {0, 0x01, 0x00};
	const uint8_t		   c[3] = {0, 0x02, 0x00};
	b.onKeepaliveAck(a, 3);
	b.onKeepaliveAck(c, 3);
	EXPECT_EQ(b.id(), 2);
}

TEST(RemoteBoot, LossClears) {
	routercore::RemoteBoot b;
	const uint8_t		   ack[3] = {0, 0x01, 0x02};
	b.onKeepaliveAck(ack, 3);
	b.forget();
	EXPECT_FALSE(b.valid());
}

TEST(StatusResp, LayoutIsSevenBytesWithBootId) {
	routercore::RemoteBoot b;
	const uint8_t		   ack[3] = {0, 0xCD, 0xAB};
	b.onKeepaliveAck(ack, 3);
	uint8_t p[16] = {};
	ASSERT_EQ(routercore::buildStatus(p, true, false, true, true, b), 7);
	EXPECT_EQ(p[0], 1);
	EXPECT_EQ(p[1], 0);
	EXPECT_EQ(p[2], 1);
	EXPECT_EQ(p[3], 1);
	EXPECT_EQ(p[4], 0x01);
	EXPECT_EQ(p[5], 0xCD);
	EXPECT_EQ(p[6], 0xAB);
}

TEST(StatusResp, InvalidBootIdGivesZeroFlagsAndId) {
	routercore::RemoteBoot b; // never seen, or v1 remote, or cleared on loss
	uint8_t				   p[16];
	std::memset(p, 0xFF, sizeof(p));
	ASSERT_EQ(routercore::buildStatus(p, false, true, false, false, b), 7);
	EXPECT_EQ(p[4], 0x00);
	EXPECT_EQ(p[5], 0x00);
	EXPECT_EQ(p[6], 0x00);
}

TEST(StatusResp, V1BaseOldPcPrefixUnchanged) {
	// an old PC reads only the first 4 bytes: they keep their meaning
	routercore::RemoteBoot b;
	uint8_t				   p[16] = {};
	routercore::buildStatus(p, true, true, false, true, b);
	EXPECT_EQ(p[0], 1);
	EXPECT_EQ(p[1], 1);
	EXPECT_EQ(p[2], 0);
	EXPECT_EQ(p[3], 1);
}

// A8: ENTER_BOOTLOADER announces the update to the remote once at once and once more 100 ms later, then stays quiet;
// the base sends no KEEPALIVE while the notice is active (the remote clears BASE_UPDATING on a KEEPALIVE).
TEST(A8, BootNoticeSendsTwiceThenStops) {
	routercore::BootNotice n;
	EXPECT_FALSE(n.active());
	EXPECT_FALSE(n.poll(1000));
	n.start(1000);
	EXPECT_TRUE(n.active());
	EXPECT_TRUE(n.poll(1000));	// at once
	EXPECT_FALSE(n.poll(1050)); // not yet
	EXPECT_FALSE(n.poll(1099));
	EXPECT_TRUE(n.poll(1100));	// the resend
	EXPECT_FALSE(n.poll(1101));
	EXPECT_FALSE(n.poll(1250)); // exactly two frames per start
	EXPECT_TRUE(n.active());	// keeps blocking KEEPALIVE until the reset
}

TEST(A8, BootNoticeAcrossClockWrap) {
	routercore::BootNotice n;
	n.start(0xFFFFFFC0u);
	EXPECT_TRUE(n.poll(0xFFFFFFC0u));
	EXPECT_FALSE(n.poll(0xFFFFFFF0u));
	EXPECT_TRUE(n.poll(0xFFFFFFC0u + 100u)); // wrapped deadline
}

TEST(A8, BaseUpdatingFlagValue) {
	// the frame the router sends: BASE_UPDATING | SONG_HAS_SECTIONS = 0x03, bit1 as in PROTOCOL.md
	EXPECT_EQ(protocol::SIGNAL_INFO_BASE_UPDATING, 0x02);
	EXPECT_EQ(protocol::SIGNAL_INFO_BASE_UPDATING | protocol::SIGNAL_INFO_SONG_HAS_SECTIONS, 0x03);
}

// C5: a signal repeated with the same PacketId is skipped; a new id, a wrap and forget() let it through.
TEST(SignalDedup, RepeatNewIdForgetAndWrap) {
	routercore::SignalDedup s;
	EXPECT_FALSE(s.isRepeat(7));
	EXPECT_TRUE(s.isRepeat(7));
	EXPECT_FALSE(s.isRepeat(8));
	EXPECT_FALSE(s.isRepeat(7)) << "only the last id counts";
	s.forget();
	EXPECT_FALSE(s.isRepeat(7)) << "a new PC session numbers from 1 again";
	EXPECT_FALSE(s.isRepeat(255));
	EXPECT_FALSE(s.isRepeat(0));
	EXPECT_TRUE(s.isRepeat(0));
}
