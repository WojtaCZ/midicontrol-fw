// Host tests for logic/ble_status: status text of the RN4870 and of the RNBD350.
// RNBD350 strings are from the bench capture of 2026-11-04 (remote module 40:84:32:04:62:5F).
#include <gtest/gtest.h>
#include "ble_status.hpp"

using namespace blestatus;

TEST(BleStatusWord, BareRn4870Form) {
	EXPECT_TRUE(isWord("DISCONNECT", "DISCONNECT"));
	EXPECT_TRUE(isWord("STREAM_OPEN", "STREAM_OPEN"));
}

TEST(BleStatusWord, Rnbd350FormWithHandle) {
	EXPECT_TRUE(isWord("DISCONNECT,0072", "DISCONNECT"));
	EXPECT_TRUE(isWord("ERR_SEC,0072", "ERR_SEC"));
}

TEST(BleStatusWord, PrefixOfALongerWordDoesNotMatch) {
	EXPECT_FALSE(isWord("DISCONNECTED", "DISCONNECT"));
	EXPECT_FALSE(isWord("ERR_SECX", "ERR_SEC"));
	EXPECT_FALSE(isWord("DISCO", "DISCONNECT"));
	EXPECT_FALSE(isWord("", "DISCONNECT"));
}

TEST(BleStatusConnect, Rn4870Form) {
	char mac[13];
	int type = -1;
	ASSERT_TRUE(parseConnect("CONNECT,0,D88039FE5996", type, mac, sizeof(mac)));
	EXPECT_EQ(type, 0);
	EXPECT_STREQ(mac, "D88039FE5996");
}

TEST(BleStatusConnect, Rnbd350FormStopsMacAtHandle) {
	char mac[13];
	int type = -1;
	ASSERT_TRUE(parseConnect("CONNECT,0,10B1DF04F934,0072", type, mac, sizeof(mac)));
	EXPECT_EQ(type, 0);
	EXPECT_STREQ(mac, "10B1DF04F934");
}

TEST(BleStatusConnect, RandomAddressType) {
	char mac[13];
	int type = -1;
	ASSERT_TRUE(parseConnect("CONNECT,1,CAD46E49E827,0040", type, mac, sizeof(mac)));
	EXPECT_EQ(type, 1);
	EXPECT_STREQ(mac, "CAD46E49E827");
}

TEST(BleStatusConnect, RejectsMalformed) {
	char mac[13];
	int type = -1;
	EXPECT_FALSE(parseConnect("CONNECT", type, mac, sizeof(mac)));
	EXPECT_FALSE(parseConnect("CONNECT,0", type, mac, sizeof(mac)));
	EXPECT_FALSE(parseConnect("CONNECT,0,", type, mac, sizeof(mac)));
	EXPECT_FALSE(parseConnect("CONNECT,x,D88039FE5996", type, mac, sizeof(mac)));
	EXPECT_FALSE(parseConnect("CONN_PARAM,0010,0000,0048,0072", type, mac, sizeof(mac)));
}

TEST(BleStatusConnect, TruncatesToBuffer) {
	char mac[5];
	int type = -1;
	ASSERT_TRUE(parseConnect("CONNECT,0,10B1DF04F934,0072", type, mac, sizeof(mac)));
	EXPECT_STREQ(mac, "10B1");
}
