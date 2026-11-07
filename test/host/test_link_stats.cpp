#include <gtest/gtest.h>

#include "link_stats.hpp"

using namespace linkstats;

TEST(LinkStats, ConnParamIsHex) {
	ConnParams p;
	ASSERT_TRUE(parseConnParam("CONN_PARAM,0010,0000,0048,0072", p));
	EXPECT_EQ(p.interval, 0x10);
	EXPECT_EQ(p.latency, 0);
	EXPECT_EQ(p.timeout, 0x48); // 720 ms
	ASSERT_TRUE(parseConnParam("CONN_PARAM,0010,0000,00FA", p)); // RN4870 form, no handle
	EXPECT_EQ(p.timeout, 250);
	EXPECT_FALSE(parseConnParam("CONN_PARAM,0010,0000", p));
	EXPECT_FALSE(parseConnParam("CONN_PARAM,00G0,0000,0048", p));
	EXPECT_FALSE(parseConnParam("CONNECT,0,AABBCCDDEEFF", p));
}

TEST(LinkStats, PhyUpdated) {
	uint8_t tx = 0, rx = 0;
	ASSERT_TRUE(parsePhyUpdated("PHY_UPDATED,0072,03,03", tx, rx));
	EXPECT_EQ(tx, 3);
	EXPECT_EQ(rx, 3);
	ASSERT_TRUE(parsePhyUpdated("PHY_UPDATED,02,01", tx, rx));
	EXPECT_EQ(tx, 2);
	EXPECT_EQ(rx, 1);
	EXPECT_FALSE(parsePhyUpdated("PHY_UPDATED,0072", tx, rx));
	EXPECT_FALSE(parsePhyUpdated("ERR_PHYUPDATE,0072", tx, rx));
}

TEST(LinkStats, RssiLine) {
	int8_t d = 0;
	ASSERT_TRUE(parseRssi("-35", d));
	EXPECT_EQ(d, -53); // hex, like the scan lines
	ASSERT_TRUE(parseRssi("-5A", d));
	EXPECT_EQ(d, -90);
	ASSERT_TRUE(parseRssi("B5", d));
	EXPECT_EQ(d, -75); // two's complement byte
	EXPECT_FALSE(parseRssi("", d));
	EXPECT_FALSE(parseRssi("Err", d));
	EXPECT_FALSE(parseRssi("CMD>", d));
	EXPECT_FALSE(parseRssi("-123", d));
}

TEST(LinkStats, CountersAndReconnectTime) {
	Stats s;
	s.onConnect();
	s.onLost(true, 10000);
	EXPECT_EQ(s.c.losses, 1);
	EXPECT_EQ(s.c.errConn, 1);
	s.onStreamOpen(10000 + 2300);
	EXPECT_EQ(s.c.lastReconnectMs, 2300);
	s.onStreamOpen(99999); // no loss pending: unchanged
	EXPECT_EQ(s.c.lastReconnectMs, 2300);
	s.onLost(false, 20000);
	EXPECT_EQ(s.c.losses, 2);
	EXPECT_EQ(s.c.errConn, 1);
	s.onStreamOpen(20000 + 200000); // saturates
	EXPECT_EQ(s.c.lastReconnectMs, 0xFFFF);
}

TEST(LinkStats, RebootInterval) {
	Stats s;
	EXPECT_EQ(s.onUnexpectedReboot(5000), 0u);
	EXPECT_EQ(s.onUnexpectedReboot(8000), 3000u);
	EXPECT_EQ(s.c.reboots, 2);
}

TEST(LinkStats, CountersSaturate) {
	Stats s;
	s.c.secureRetries = 0xFFFF;
	s.onSecureRetry();
	EXPECT_EQ(s.c.secureRetries, 0xFFFF);
}

TEST(LinkStats, RssiGoesStaleAndClearsOnLoss) {
	Stats s;
	EXPECT_EQ(s.rssiNow(0), RSSI_UNKNOWN);
	s.setRssi(-60, 1000);
	EXPECT_EQ(s.rssiNow(2000), -60);
	EXPECT_EQ(s.rssiNow(1000 + RSSI_STALE_MS + 1), RSSI_UNKNOWN);
	s.onLost(false, 3000);
	EXPECT_EQ(s.rssiNow(3001), RSSI_UNKNOWN);
}

TEST(LinkStats, StatusHandler) {
	Stats s;
	EXPECT_TRUE(s.onStatus("CONN_PARAM,0010,0000,0048,0072"));
	EXPECT_EQ(s.params.timeout, 0x48);
	EXPECT_TRUE(s.onStatus("PHY_UPDATED,0072,03,03"));
	EXPECT_EQ(s.phyTx, 3);
	EXPECT_FALSE(s.onStatus("REBOOT"));
	s.onConnect();
	EXPECT_EQ(s.phyTx, 0);
	EXPECT_EQ(s.params.timeout, 0);
}

TEST(LinkStats, ConnectHandle) {
	char h[8] = "";
	ASSERT_TRUE(connectHandle("CONNECT,0,10B1DF04F934,0072", h, sizeof(h)));
	EXPECT_STREQ(h, "0072");
	EXPECT_FALSE(connectHandle("CONNECT,0,10B1DF04F934", h, sizeof(h))); // RN4870, no handle
	EXPECT_FALSE(connectHandle("DISCONNECT,0072", h, sizeof(h)));
}
