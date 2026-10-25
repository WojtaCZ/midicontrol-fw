#pragma once
// Host stand-in for inc/bluetooth.hpp (the real one needs the GPIO/USART register layer)
#include <cstdint>
#include <stmcpp/units.hpp>
#include <stmcpp/systick.hpp> // the real header brings it in for menu.cpp

namespace Bluetooth {
	bool isUp();
	uint8_t startPairing();
	bool isConnected();
	stmcpp::units::duration getDataReceivedTimestamp();
}
