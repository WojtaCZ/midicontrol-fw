// Pins the protocol CRC: poly 0x31, init 0x00, MSB first, no reflection, no final XOR. The check value of
// "123456789" is 0xA2. This is NOT CRC-8/MAXIM (reflected, check 0xA1).
#include <gtest/gtest.h>
#include "protocol/crc8.hpp"

TEST(Crc8, CheckValue123456789) {
	const uint8_t data[] = {'1', '2', '3', '4', '5', '6', '7', '8', '9'};
	EXPECT_EQ(protocol::crc8(data, sizeof(data)), 0xA2);
}

TEST(Crc8, EmptyIsInit) {
	EXPECT_EQ(protocol::crc8(nullptr, 0), 0x00);
}
