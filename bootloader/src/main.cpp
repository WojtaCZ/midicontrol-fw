#include <stm32g431xx.h>
#include <core_cm4.h>
#include <cmsis_compiler.h>

#include <tusb.h>
#include <tinyusb/src/class/cdc/cdc_device.h>

#include <stmcpp/register.hpp>
#include <stmcpp/gpio.hpp>

#include "protocol/parser.hpp"
#include "protocol/codec.hpp"
#include "protocol/types.hpp"
#include "protocol/constants.hpp"

#include "boot/layout.hpp"
#include "boot/engine.hpp"
#include "boot/flashprog.hpp"
#include "boot/crc32.hpp"
#include "boot/fw_receiver.hpp"

// Base USB bootloader. Stays resident when the application set the backup-register
// magic before resetting; brings up USB-CDC and programs the application region
// directly from the FW_UPDATE_* stream, then resets into the new application.

static constexpr uint32_t kEnterBootMagic = 0xB00710AD; // must match the application

namespace {

	// Sink that programs the incoming image straight into the internal application
	// region. Flash is unlocked across the transfer and re-locked at finish.
	class FlashSink : public boot::FwSink {
	public:
		bool begin(uint32_t size, uint32_t crc) override {
			if (size == 0 || size > boot::kAppMaxSize) return false;
			_size = size;
			_crc = crc;
			_written = 0;
			boot::flashprog::unlock();
			return boot::flashprog::eraseRange(boot::kAppBase, size);
		}
		bool write(uint32_t offset, const uint8_t *data, uint32_t len) override {
			if (offset + len > _size) return false;
			// Idempotent against retransmits: a chunk already programmed (its ACK was
			// lost) must not be reprogrammed — that would be a PROGERR on already
			// written flash. Chunks arrive strictly sequentially.
			if (offset < _written) return true;       // duplicate
			if (offset != _written) return false;     // unexpected gap
			if (!boot::flashprog::program(boot::kAppBase + offset, data, len)) return false;
			_written = offset + len;
			return true;
		}
		bool finish() override {
			boot::flashprog::lock();
			return boot::crc32(reinterpret_cast<const uint8_t *>(boot::kAppBase), _size) == _crc;
		}
	private:
		uint32_t _size = 0;
		uint32_t _crc = 0;
		uint32_t _written = 0;
	};

	FlashSink s_sink;
	uint8_t s_packetId = 0;
	boot::FwReceiver s_rx(s_sink, protocol::ADDR_BASE, s_packetId);
	protocol::Parser s_parser;
	bool s_applyRequested = false;

	void sendPacket(const protocol::Packet &pkt) {
		uint8_t buf[protocol::MAX_FRAME_SIZE];
		uint16_t len = protocol::encode(pkt, buf);
		if (len > 0) {
			tud_cdc_write(buf, len);
			tud_cdc_write_flush();
		}
	}

	void onPacket(const protocol::Packet &in) {
		protocol::Packet out;
		bool hasOut = false;
		auto action = s_rx.handle(in, out, hasOut);
		if (action == boot::FwReceiver::Action::None) return;
		if (hasOut) sendPacket(out);
		if (action == boot::FwReceiver::Action::Apply) s_applyRequested = true;
	}

	// Bring up the 48 MHz USB clock from HSI48 (core stays on the reset HSI16).
	void usbClockInit() {
		using namespace stmcpp;

		reg::set(std::ref(RCC->CRRCR), RCC_CRRCR_HSI48ON);
		while (!(RCC->CRRCR & RCC_CRRCR_HSI48RDY)) {}

		reg::set(std::ref(RCC->APB1ENR1), RCC_APB1ENR1_PWREN | RCC_APB1ENR1_CRSEN |
											RCC_APB1ENR1_USBEN | RCC_APB1ENR1_RTCAPBEN);

		// CLK48 source = HSI48.
		reg::change(std::ref(RCC->CCIPR), RCC_CCIPR_CLK48SEL_Msk, (0b00u << RCC_CCIPR_CLK48SEL_Pos));

#if defined(PWR_CR2_USV)
		PWR->CR2 |= PWR_CR2_USV; // validate the independent USB supply
#endif
		PWR->CR1 |= PWR_CR1_DBP;  // allow backup-register access

		// Trim HSI48 against USB SOF.
		CRS->CFGR &= ~CRS_CFGR_SYNCSRC;
		CRS->CFGR |= (0b10u << CRS_CFGR_SYNCSRC_Pos);
		CRS->CR |= CRS_CR_AUTOTRIMEN;
		CRS->CR |= CRS_CR_CEN;

		reg::set(std::ref(RCC->AHB2ENR), RCC_AHB2ENR_GPIOAEN);
	}

} // namespace

extern "C" void SystemInit(void) {
	SCB->VTOR = 0x08000000;
	#if (__FPU_PRESENT == 1) && (__FPU_USED == 1)
		SCB->CPACR |= (3UL << 20U) | (3UL << 22U);
	#endif
}

extern "C" int main(void) {
	usbClockInit();

	// Decide whether to stay in the bootloader. The application sets the magic
	// before resetting to request a firmware update; clear it so the next normal
	// reset runs the application again.
	bool stay = (TAMP->BKP0R == kEnterBootMagic);
	TAMP->BKP0R = 0;

	if (!stay && boot::appValid(boot::kAppBase)) {
		boot::jumpToApp(boot::kAppBase);
	}

	// USB data pins (PA11 = D-, PA12 = D+).
	stmcpp::gpio::pin<stmcpp::gpio::port::porta, 11> usb_dm(stmcpp::gpio::mode::input, stmcpp::gpio::otype::pushPull, stmcpp::gpio::speed::veryHigh, stmcpp::gpio::pull::noPull);
	stmcpp::gpio::pin<stmcpp::gpio::port::porta, 12> usb_dp(stmcpp::gpio::mode::input, stmcpp::gpio::otype::pushPull, stmcpp::gpio::speed::veryHigh, stmcpp::gpio::pull::noPull);

	tusb_init();
	NVIC_EnableIRQ(USB_LP_IRQn);
	NVIC_EnableIRQ(USB_HP_IRQn);

	while (1) {
		tud_task();

		if (tud_cdc_available()) {
			uint8_t buf[64];
			uint32_t n = tud_cdc_read(buf, sizeof(buf));
			for (uint32_t i = 0; i < n; ++i) {
				if (s_parser.feed(buf[i])) {
					onPacket(s_parser.getPacket());
				}
			}
		}

		if (s_applyRequested) {
			// Flush the final ACK, then reset into the freshly programmed app.
			for (volatile uint32_t i = 0; i < 400000; ++i) {
				tud_task();
			}
			NVIC_SystemReset();
		}
	}
}

extern "C" void USB_HP_IRQHandler(void) { tud_int_handler(0); }
extern "C" void USB_LP_IRQHandler(void) { tud_int_handler(0); }
extern "C" void USBWakeUp_IRQHandler(void) { tud_int_handler(0); }
