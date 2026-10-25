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
#include "boot/image.hpp"
#include "boot/wdg.hpp"
#include "boot/oled.hpp"

// Base USB bootloader. Stays resident when
//   * the application set the backup-register magic before resetting (update
//     requested over USB, ENTER_BOOTLOADER), or
//   * the encoder button (PB1) is held down at reset (forced entry, e.g. when the
//     application crashes before it can handle ENTER_BOOTLOADER), or
//   * the application region fails the boot check (blank, interrupted update,
//     bad trailer / CRC, image for the remote).
// It brings up USB-CDC (VID:PID ABCD:1235, the app's PID + 1) and programs the
// application region from the FW_UPDATE_* stream, then resets into the new app.
// The ST ROM DFU loader stays reachable through BOOT0 (jumper JP1).

static constexpr uint32_t kEnterBootMagic = 0xB00710AD; // must match the application

namespace {

	// Sink that programs the incoming image into the internal application region.
	//
	// Power-loss safety: begin() erases the whole region, vector-table page first.
	// The first page is collected in RAM and programmed only in finish(), after
	// FwReceiver has checked the file CRC and the image trailer (board ID, size,
	// image CRC). Until then the region has no vector table, so an interrupted or
	// rejected update leaves a region that appValid() refuses — the bootloader
	// keeps running and the PC can simply retry.
	//
	// FwReceiver delivers chunks strictly in order and without duplicates.
	class FlashSink : public boot::FwSink {
	public:
		bool begin(uint32_t size, uint32_t crc) override {
			if (size == 0 || size > boot::kAppMaxSize) return false;
			_size = size;
			_crc = crc;
			for (auto &b : _page0) b = 0xFF;
			boot::flashprog::unlock();
			return boot::flashprog::eraseRange(boot::kAppBase, boot::kAppMaxSize);
		}
		bool write(uint32_t offset, const uint8_t *data, uint32_t len) override {
			if (offset + len > _size) return false;
			// Doubleword programming: every chunk but the last must be a multiple of
			// 8 bytes; the last one is padded with 0xFF by flashprog::program().
			if ((offset % 8u) != 0 || ((len % 8u) != 0 && offset + len != _size)) return false;

			// Part that falls into the vector-table page: keep it for finish().
			while (len > 0 && offset < boot::kPageSize) {
				_page0[offset++] = *data++;
				--len;
			}
			if (len == 0) return true;
			return boot::flashprog::program(boot::kAppBase + offset, data, len);
		}
		bool finish() override {
			const uint32_t firstLen = _size < boot::kPageSize ? _size : boot::kPageSize;
			const auto *app = reinterpret_cast<const uint8_t *>(boot::kAppBase);

			// What is in flash + RAM must be what the PC sent before it becomes bootable.
			boot::Crc32 c;
			c.update(_page0, firstLen);
			c.update(app + firstLen, _size - firstLen);
			bool ok = c.value() == _crc;

			// Commit: the vector table goes in last.
			ok = ok && boot::flashprog::program(boot::kAppBase, _page0, firstLen);
			boot::flashprog::lock();
			return ok && boot::crc32(app, _size) == _crc;
		}
	private:
		uint32_t _size = 0;
		uint32_t _crc = 0;
		uint8_t _page0[boot::kPageSize];
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

	// Hodiny a piny pro OLED (I2C1 na APB1, zdroj PCLK1 = HSI16 - odpovida TIMINGR nize).
	// Cekani na napajeni panelu dela boot::oled::init sam.
	void displayInit() {
		using namespace stmcpp;
		reg::set(std::ref(RCC->AHB2ENR), RCC_AHB2ENR_GPIOAEN | RCC_AHB2ENR_GPIOBEN);
		reg::set(std::ref(RCC->APB1ENR1), RCC_APB1ENR1_I2C1EN);
		reg::change(std::ref(RCC->CCIPR), RCC_CCIPR_I2C1SEL_Msk, (0b00u << RCC_CCIPR_I2C1SEL_Pos));

		gpio::pin<gpio::port::porta, 15> scl(gpio::mode::af4, gpio::otype::openDrain, gpio::speed::veryHigh, gpio::pull::pullUp);
		gpio::pin<gpio::port::portb, 7> sda(gpio::mode::af4, gpio::otype::openDrain, gpio::speed::veryHigh, gpio::pull::pullUp);

		boot::oled::init(0x10320309); // 400 kHz @ 16 MHz (RM0440)
		boot::oled::show("Aktualizace", "Cekam na PC", 0);
	}

	// Casovac vypadku PC: SysTick v bootloaderu nebezi, tak DWT->CYCCNT (jadro zustava na HSI16 = 16 MHz,
	// 20 s = 320 M taktu < 2^32). Rozdil uint32_t je odolny proti preteceni.
	constexpr uint32_t kStallCycles = 20u * 16000000u;
	uint32_t s_lastPacketCycles = 0;
	bool s_stallShown = false;

	void cycleCounterInit() {
		CoreDebug->DEMCR |= CoreDebug_DEMCR_TRCENA_Msk;
		DWT->CYCCNT = 0;
		DWT->CTRL |= DWT_CTRL_CYCCNTENA_Msk;
	}

	void showProgress(const char *hint, uint8_t percent) {
		boot::oled::show("Aktualizace", hint, percent);
	}

	uint8_t progressPercent() {
		const uint32_t size = s_rx.expectedSize();
		if (size == 0) return 0;
		return static_cast<uint8_t>(static_cast<uint64_t>(s_rx.received()) * 100u / size);
	}

	void onPacket(const protocol::Packet &in) {
		protocol::Packet out;
		bool hasOut = false;
		auto action = s_rx.handle(in, out, hasOut);
		if (action == boot::FwReceiver::Action::None) return;
		s_lastPacketCycles = DWT->CYCCNT;
		s_stallShown = false;
		if (hasOut) sendPacket(out);
		if (s_rx.takeStatus(out)) sendPacket(out); // FW_UPDATE_STATUS after the ACK/NAK

		// Display after the replies (the redraw must not delay the ACK); show() writes only changed pages.
		if (action == boot::FwReceiver::Action::Failed) {
			showProgress("Chyba, opakujte", progressPercent());
		} else if (action == boot::FwReceiver::Action::Apply) {
			showProgress("Nevypinejte", 100);
		} else if (s_rx.active()) {
			showProgress("Nevypinejte", progressPercent());
		}
		if (action == boot::FwReceiver::Action::Apply) s_applyRequested = true;
	}

	// Encoder push button (PB1, active low, internal pull-up) held at reset.
	bool forcedEntry() {
		using namespace stmcpp;
		reg::set(std::ref(RCC->AHB2ENR), RCC_AHB2ENR_GPIOBEN);
		gpio::pin<gpio::port::portb, 1> button(gpio::mode::input, gpio::pull::pullUp);
		for (volatile uint32_t i = 0; i < 2000; ++i) {} // let the pull-up settle

		// Must read pressed for ~20 ms in a row (debounce, ignore a glitch).
		for (uint32_t i = 0; i < 2000; ++i) {
			if (GPIOB->IDR & GPIO_IDR_ID1) return false;
			for (volatile uint32_t d = 0; d < 40; ++d) {}
		}
		return true;
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
	stay = stay || forcedEntry();

	boot::wdg::kick();
	if (!stay && boot::appBootable(boot::image::kBoardBase)) {
		boot::jumpToApp(boot::kAppBase);
	}

	cycleCounterInit();
	displayInit(); // only on the resident path; a jump to a valid app stays fast and dark

	// USB data pins (PA11 = D-, PA12 = D+).
	stmcpp::gpio::pin<stmcpp::gpio::port::porta, 11> usb_dm(stmcpp::gpio::mode::input, stmcpp::gpio::otype::pushPull, stmcpp::gpio::speed::veryHigh, stmcpp::gpio::pull::noPull);
	stmcpp::gpio::pin<stmcpp::gpio::port::porta, 12> usb_dp(stmcpp::gpio::mode::input, stmcpp::gpio::otype::pushPull, stmcpp::gpio::speed::veryHigh, stmcpp::gpio::pull::noPull);

	tusb_init();
	NVIC_EnableIRQ(USB_LP_IRQn);
	NVIC_EnableIRQ(USB_HP_IRQn);

	while (1) {
		boot::wdg::kick(); // IWDG z aplikace běží i po ENTER_BOOTLOADER a nejde zastavit
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

		// PC se odmlcel uprostred prenosu (kabel, zruseni): vyzvat k opakovani, ukazatel zustane.
		// Dalsi paket se vrati na "Nevypinejte" v onPacket().
		if (s_rx.active() && !s_stallShown && DWT->CYCCNT - s_lastPacketCycles > kStallCycles) {
			s_stallShown = true;
			showProgress("Chyba, opakujte", progressPercent());
		}

		if (s_applyRequested) {
			// Flush the final ACK, then reset into the freshly programmed app.
			for (volatile uint32_t i = 0; i < 400000; ++i) {
				boot::wdg::kick();
				tud_task();
			}
			NVIC_SystemReset();
		}
	}
}

extern "C" void USB_HP_IRQHandler(void) { tud_int_handler(0); }
extern "C" void USB_LP_IRQHandler(void) { tud_int_handler(0); }
extern "C" void USBWakeUp_IRQHandler(void) { tud_int_handler(0); }
