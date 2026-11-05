// Host tests for logic/ble_status: status text of the RN4870 and of the RNBD350.
// RNBD350 strings are from the bench capture of 2026-11-04 (remote module 40:84:32:04:62:5F).
#include <gtest/gtest.h>
#include "ble_status.hpp"
#include <cstring>
#include <string>

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

// Module self-test shown on the Bluetooth info page (bench check without a probe after soldering)
TEST(BleHealth, StartingWhileTheConfigurationRuns) {
	EXPECT_EQ(health(true, false, false, 0), Health::STARTING);
}

TEST(BleHealth, OkWhenConfiguredAfterAResetPulse) {
	EXPECT_EQ(health(false, true, true, 120), Health::OK);
}

TEST(BleHealth, ResetLineBrokenButCommandsWork) {
	EXPECT_EQ(health(false, true, false, 120), Health::OK_NO_RESET);
}

TEST(BleHealth, ModuleSendsButDoesNotAnswer) {
	EXPECT_EQ(health(false, false, false, 59), Health::NO_REPLY);
	EXPECT_EQ(health(false, false, true, 10), Health::NO_REPLY);
}

TEST(BleHealth, NothingEverReceived) {
	EXPECT_EQ(health(false, false, false, 0), Health::SILENT);
}

TEST(BleHealth, TextsFitTheDisplayLine) {
	for (Health h : {Health::STARTING, Health::OK, Health::OK_NO_RESET, Health::NO_REPLY, Health::SILENT}) {
		const char *t = healthText(h);
		ASSERT_NE(t, nullptr);
		EXPECT_GT(strlen(t), 0u);
		EXPECT_LE(strlen(t), 18u) << t; // 7x10 font: 18 characters per line
	}
	EXPECT_STREQ(healthText(Health::OK), "Modul: OK");
}

// ── Scan results (F) ─────────────────────────────────────────────────────
// RNBD350 lines are from the base's scan on the bench, 2026-11-05 (remote module 40:84:32:04:62:5F).

static std::string str(ScanField f) { return std::string(f.p, f.len); }

TEST(BleScan, Rn4870Connectable) {
	ScanResult r;
	ASSERT_TRUE(parseScan("D88039FE5996,0,MIDIControlRemote,,F5", r));
	EXPECT_TRUE(r.connectable);
	EXPECT_EQ(str(r.addr), "D88039FE5996");
	EXPECT_EQ(r.type, 0);
	EXPECT_EQ(str(r.name), "MIDIControlRemote");
	EXPECT_EQ(str(r.uuids), "");
	EXPECT_EQ(r.rssi, -11);
}

TEST(BleScan, Rn4870NonConnectable) {
	ScanResult r;
	ASSERT_TRUE(parseScan("CAD46E49E827,1,AD,Brcst:07FF4C0012020001", r));
	EXPECT_FALSE(r.connectable);
	EXPECT_EQ(str(r.addr), "CAD46E49E827");
	EXPECT_EQ(r.type, 1);
	EXPECT_EQ(r.rssi, -83);
	EXPECT_EQ(str(r.data), "07FF4C0012020001");
}

TEST(BleScan, Rnbd350ConnectableWithUuid) {
	ScanResult r;
	ASSERT_TRUE(parseScan("40843204625F,00,MIDIControlRemote,FEDA,-35,01,00", r));
	EXPECT_TRUE(r.connectable);
	EXPECT_EQ(str(r.addr), "40843204625F");
	EXPECT_EQ(r.type, 0);
	EXPECT_EQ(str(r.name), "MIDIControlRemote");
	EXPECT_EQ(str(r.uuids), "FEDA");
	EXPECT_EQ(r.rssi, -0x35);
}

TEST(BleScan, Rnbd350ScanResponseWithoutUuid) {
	ScanResult r;
	ASSERT_TRUE(parseScan("40843204625F,00,MIDIControlRemote,,-35,01,00", r));
	EXPECT_EQ(str(r.name), "MIDIControlRemote");
	EXPECT_EQ(str(r.uuids), "");
}

TEST(BleScan, Rnbd350NamelessRandomAddress) {
	ScanResult r;
	ASSERT_TRUE(parseScan("50420F080495,01,,,-48,01,00", r));
	EXPECT_TRUE(r.connectable);
	EXPECT_EQ(r.type, 1);
	EXPECT_EQ(str(r.name), "");
}

TEST(BleScan, Rnbd350NameWithSpacesAndParentheses) {
	ScanResult r;
	ASSERT_TRUE(parseScan("D49DC04F6255,00,[TV] Samsung 7 Series (43),,-47,01,00", r));
	EXPECT_EQ(str(r.name), "[TV] Samsung 7 Series (43)");
}

TEST(BleScan, Rnbd350NonConnectable) {
	ScanResult r;
	ASSERT_TRUE(parseScan("8C79F5D3DE29,00,-59,01,00,Brcst:1BFF750042040180608C79F5D3DE", r));
	EXPECT_FALSE(r.connectable);
	EXPECT_EQ(str(r.addr), "8C79F5D3DE29");
	EXPECT_EQ(r.type, 0);
	EXPECT_EQ(r.rssi, -0x59);
	EXPECT_EQ(str(r.data), "1BFF750042040180608C79F5D3DE");
}

TEST(BleScan, RejectsOtherStatusText) {
	ScanResult r;
	EXPECT_FALSE(parseScan("CONNECT,0,10B1DF04F934,0072", r));
	EXPECT_FALSE(parseScan("STREAM_OPEN", r));
	EXPECT_FALSE(parseScan("", r));
	EXPECT_FALSE(parseScan("40843204625,00,X,,-35,01,00", r));   // 11-digit address
	EXPECT_FALSE(parseScan("40843204625F,2,X,,-35", r));         // type 2
	EXPECT_FALSE(parseScan("40843204625F,00,X,,-35,01", r));     // 6 fields, no Brcst
	EXPECT_FALSE(parseScan("40843204625F,00,X,,ZZ,01,00", r));   // RSSI not hex
}
