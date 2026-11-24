// Pairing confirmation handshake (protocol/pairing.hpp), 2026-11-24: a newly secured peer is only stored after both users said Ano.
#include <gtest/gtest.h>
#include "protocol/pairing.hpp"
#include "protocol/types.hpp"

using namespace protocol::pairing;

static_assert(T_REQ == static_cast<uint8_t>(protocol::MsgType::PAIR_REQ));
static_assert(T_RSP == static_cast<uint8_t>(protocol::MsgType::PAIR_RSP));
static_assert(T_CONFIRM == static_cast<uint8_t>(protocol::MsgType::PAIR_CONFIRM));

namespace {
	const uint8_t NB[4] = {1, 2, 3, 4}, NR[4] = {9, 8, 7, 6};

	struct Pair {
		Session base, remote;
		uint8_t req[8], rsp[8];
		void begin(uint32_t t = 0) {
			base.start(Role::INITIATOR, NB, t);
			remote.start(Role::RESPONDER, NR, t);
		}
		// REQ -> RSP exchange; both sit in CONFIRM afterwards
		void exchange(uint32_t t = 10) {
			size_t n = base.hello(req);
			ASSERT_EQ(remote.onFrame(T_REQ, req, n, t), Step::REPLY_AND_SHOW);
			n = remote.hello(rsp);
			ASSERT_EQ(base.onFrame(T_RSP, rsp, n, t), Step::SHOW);
		}
	};
}

TEST(PairSession, CodeIsSymmetricAndSixDigits) {
	Pair p;
	p.begin();
	p.exchange();
	EXPECT_EQ(p.base.code(), p.remote.code());
	EXPECT_LT(p.base.code(), CODE_MODULO);
	EXPECT_EQ(p.base.code(), code(NB, NR));
}

TEST(PairSession, NonceDiffersWithSeeds) {
	uint8_t a[4], b[4];
	makeNonce(1, 2, a);
	makeNonce(1, 3, b);
	EXPECT_NE(0, memcmp(a, b, 4));
}

TEST(PairSession, BothAnoSucceeds) {
	Pair p;
	p.begin();
	p.exchange();
	uint8_t c[8];
	EXPECT_EQ(p.base.onLocal(true), Step::NONE);
	size_t n = p.base.confirm(true, c);
	EXPECT_EQ(p.remote.onFrame(T_CONFIRM, c, n, 20), Step::NONE); // remote user has not decided yet
	EXPECT_EQ(p.remote.onLocal(true), Step::SUCCESS);
	n = p.remote.confirm(true, c);
	EXPECT_EQ(p.base.onFrame(T_CONFIRM, c, n, 25), Step::SUCCESS);
	EXPECT_EQ(p.base.state(), State::SUCCESS);
	EXPECT_EQ(p.remote.state(), State::SUCCESS);
}

TEST(PairSession, LocalNeFailsAndNotifiesPeer) {
	Pair p;
	p.begin();
	p.exchange();
	EXPECT_EQ(p.remote.onLocal(false), Step::FAIL);
	EXPECT_EQ(p.remote.fail(), Fail::REJECTED_LOCAL);
	EXPECT_TRUE(p.remote.notifyPeer());
	uint8_t c[8];
	size_t n = p.remote.confirm(false, c);
	EXPECT_EQ(p.base.onFrame(T_CONFIRM, c, n, 20), Step::FAIL);
	EXPECT_EQ(p.base.fail(), Fail::REJECTED_PEER);
	EXPECT_FALSE(p.base.notifyPeer());
}

TEST(PairSession, WrongCodeIsMismatch) {
	Pair p;
	p.begin();
	p.exchange();
	uint8_t c[8];
	size_t n = p.base.confirm(true, c);
	c[1] ^= 1;
	EXPECT_EQ(p.remote.onFrame(T_CONFIRM, c, n, 20), Step::FAIL);
	EXPECT_EQ(p.remote.fail(), Fail::MISMATCH);
}

TEST(PairSession, PeerSilentFailsAfterReplyTimeout) {
	Pair p;
	p.begin(100);
	EXPECT_EQ(p.base.poll(100 + REPLY_TIMEOUT_MS - 1), Step::NONE);
	EXPECT_EQ(p.base.poll(100 + REPLY_TIMEOUT_MS), Step::FAIL);
	EXPECT_EQ(p.base.fail(), Fail::TIMEOUT);
	EXPECT_FALSE(p.base.notifyPeer()); // the code was never known
}

TEST(PairSession, UserSilentFailsAfterConfirmTimeout) {
	Pair p;
	p.begin(0);
	p.exchange(10);
	EXPECT_EQ(p.base.poll(10 + CONFIRM_TIMEOUT_MS - 1), Step::NONE);
	EXPECT_EQ(p.base.poll(10 + CONFIRM_TIMEOUT_MS), Step::FAIL);
	EXPECT_TRUE(p.base.notifyPeer());
}

TEST(PairSession, UnexpectedOrMalformedFramesFail) {
	Pair p;
	p.begin();
	uint8_t bad[5] = {2, 0, 0, 0, 0}; // wrong version
	EXPECT_EQ(p.remote.onFrame(T_REQ, bad, 5, 5), Step::FAIL);
	EXPECT_EQ(p.remote.fail(), Fail::BAD_FRAME);
	Session s;
	s.start(Role::INITIATOR, NB, 0);
	EXPECT_EQ(s.onFrame(0x10, bad, 5, 1), Step::FAIL); // a normal frame before the handshake ended
	EXPECT_EQ(s.fail(), Fail::BAD_FRAME);
	Session r;
	r.start(Role::INITIATOR, NB, 0);
	EXPECT_EQ(r.onFrame(T_REQ, bad, 5, 1), Step::FAIL); // initiator never accepts REQ
}

TEST(PairSession, IdleSessionIgnoresFrames) {
	Session s;
	uint8_t c[5] = {1, 0, 0, 0, 0};
	EXPECT_EQ(s.onFrame(T_CONFIRM, c, 5, 0), Step::NONE);
	EXPECT_EQ(s.state(), State::IDLE);
}
