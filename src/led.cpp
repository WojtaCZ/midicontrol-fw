#include "led.hpp"
#include "base.hpp"
#include "settings.hpp"
#include "main.hpp"
#include "display.hpp"
#include "bluetooth.hpp"
#include <cstring>
#include <core_cm4.h>
#include <cmsis_compiler.h>
#include  <stm32g431xx.h>

#include "stmcpp/register.hpp"
#include "stmcpp/gpio.hpp"
#include "stmcpp/systick.hpp"
#include "stmcpp/units.hpp"

using namespace stmcpp::units;

namespace LED{

	// Sledování aktivity — timestamp posledního notifyActivity() pro každý pixel
	static duration activityTimestamp[6] = {};
	
	static constexpr duration BLINK_DURATION = 100_ms;

	constexpr uint16_t timerPeriod = (144e6 / 800000);	// 144MHz clock, 800kHz LED strip
	constexpr uint16_t resetSlots = 200; 		// Reset slots for terminating the transmission and also setting the refresh rate
	constexpr uint16_t zeroPeriod = (timerPeriod / 3);		// 1/3 of the period for a zero bit
	constexpr uint16_t onePeriod = (timerPeriod * 2 / 3);	// 2/3 of the period for a one bit



	LED::Strip<6> rearStrip(std::array{
		Pixel(LED::Color(0, 0, 0, 0)),       // USB
		Pixel(LED::Color(0, 0, 0, 0)),       // Display
		Pixel(LED::Color(0, 0, 0, 0)),       // Current
		Pixel(LED::Color(255, 0, 0, 0.1)),   // MIDI A
		Pixel(LED::Color(0, 255, 0, 0.1)),   // MIDI B
		Pixel(LED::Color(0, 0, 0, 0)),       // Bluetooth
	});

	
	std::array frontPixels = {Pixel(LED::Color(0, 0, 0, 0)), Pixel(LED::Color(0, 0, 0, 0)), Pixel(LED::Color(0, 0, 0, 0)), Pixel(LED::Color(0, 0, 0, 0))};
	LED::Strip<4> frontStrip(frontPixels);

	uint8_t rearBuffer[2*resetSlots + 6 * 24]; // Reset slots and 3 bytes per pixel
	uint8_t frontBuffer[2*resetSlots + frontPixels.size() * 24]; // Reset slots and 3 bytes per pixel

	void init(){

		// Rear LEDs
		stmcpp::gpio::pin<stmcpp::gpio::port::portb, 5> ledStripRear (stmcpp::gpio::mode::af10, stmcpp::gpio::otype::pushPull, stmcpp::gpio::speed::low, stmcpp::gpio::pull::noPull);
		// Front LEDs
		stmcpp::gpio::pin<stmcpp::gpio::port::portb, 14> ledStripFront (stmcpp::gpio::mode::af1, stmcpp::gpio::otype::pushPull, stmcpp::gpio::speed::low, stmcpp::gpio::pull::noPull);

		stmcpp::reg::set(std::ref(TIM17->CR1), TIM_CR1_CEN); // Enable counter
        stmcpp::reg::write(std::ref(DMA2_Channel1->CCR), 
            (0b1 << DMA_CCR_DIR_Pos) | // Memory to peripheral direction
            (0b1 << DMA_CCR_MINC_Pos) | // Memory increment mode
            (0b1 << DMA_CCR_CIRC_Pos) |  // Circular DMA
			(0b01 << DMA_CCR_PSIZE_Pos) | // 16bit peripheral
			DMA_CCR_TCIE
        );

        stmcpp::reg::write(std::ref(DMA2_Channel1->CMAR), reinterpret_cast<uint32_t>(&rearBuffer[0]));
        stmcpp::reg::write(std::ref(DMA2_Channel1->CPAR), reinterpret_cast<uint32_t>(&TIM17->CCR1));
        stmcpp::reg::write(std::ref(DMA2_Channel1->CNDTR), sizeof(rearBuffer));

        // Set up DMAMUX routing (DMAMUX channels map to 6-11 to DMA2 1-6, thats why the offset in numbering)
        stmcpp::reg::write(std::ref(DMAMUX1_Channel6->CCR), (84 << DMAMUX_CxCR_DMAREQ_ID_Pos));


		// Timer setup for back LED
		RCC->APB2ENR |= RCC_APB2ENR_TIM17EN; // Enable  TIM17 clock

		stmcpp::reg::write(std::ref(TIM17->CR1), TIM_CR1_ARPE | TIM_CR1_URS); // Auto-reload preload and update generation enable
		stmcpp::reg::write(std::ref(TIM17->CR2), TIM_CR2_CCDS); // Capture/compare DMA selection
		stmcpp::reg::write(std::ref(TIM17->DIER), TIM_DIER_CC1DE); // DMA request on update event
		stmcpp::reg::write(std::ref(TIM17->EGR), TIM_EGR_CC1G); // DMA request on update event
		stmcpp::reg::write(std::ref(TIM17->CCMR1), 0b110, TIM_CCMR1_OC1M_Pos); // Set PWM mode 1
		stmcpp::reg::write(std::ref(TIM17->CCER), TIM_CCER_CC1E); // Enable output
		stmcpp::reg::write(std::ref(TIM17->BDTR), TIM_BDTR_MOE); // Main output enable
		stmcpp::reg::write(std::ref(TIM17->ARR), timerPeriod - 1); // Set auto-reload value

		// DMA setup for front LED
		stmcpp::reg::write(std::ref(DMA2_Channel2->CCR),
			(0b1 << DMA_CCR_DIR_Pos) | // Memory to peripheral direction
			(0b1 << DMA_CCR_MINC_Pos) | // Memory increment mode
			(0b1 << DMA_CCR_CIRC_Pos) | // Circular DMA
			(0b01 << DMA_CCR_PSIZE_Pos) | // 16bit peripheral
			DMA_CCR_TCIE
		);

		stmcpp::reg::write(std::ref(DMA2_Channel2->CMAR), reinterpret_cast<uint32_t>(&frontBuffer[0]));
		stmcpp::reg::write(std::ref(DMA2_Channel2->CPAR), reinterpret_cast<uint32_t>(&TIM15->CCR1));
		stmcpp::reg::write(std::ref(DMA2_Channel2->CNDTR), sizeof(frontBuffer));

		// Set up DMAMUX routing for TIM15_CH1
		stmcpp::reg::write(std::ref(DMAMUX1_Channel7->CCR), (78 << DMAMUX_CxCR_DMAREQ_ID_Pos));

		// Timer setup for front LED
		RCC->APB2ENR |= RCC_APB2ENR_TIM15EN; // Enable TIM15 clock

		stmcpp::reg::write(std::ref(TIM15->CR1), TIM_CR1_ARPE | TIM_CR1_URS); // Auto-reload preload and update generation enable
		stmcpp::reg::write(std::ref(TIM15->CR2), TIM_CR2_CCDS); // Capture/compare DMA selection
		stmcpp::reg::write(std::ref(TIM15->DIER), TIM_DIER_CC1DE); // DMA request on update event
		stmcpp::reg::write(std::ref(TIM15->EGR), TIM_EGR_CC1G); // DMA request on update event
		stmcpp::reg::write(std::ref(TIM15->CCMR1), 0b110, TIM_CCMR1_OC1M_Pos); // Set PWM mode 1
		stmcpp::reg::write(std::ref(TIM15->CCER), TIM_CCER_CC1E); // Enable output
		stmcpp::reg::write(std::ref(TIM15->BDTR), TIM_BDTR_MOE); // Main output enable
		stmcpp::reg::write(std::ref(TIM15->ARR), timerPeriod - 1); // Set auto-reload value

		NVIC_EnableIRQ(DMA2_Channel1_IRQn); // Enable DMA2 Channel 1 IRQ
		NVIC_EnableIRQ(DMA2_Channel2_IRQn); // Enable DMA2 Channel 2 IRQ

		stmcpp::reg::set(std::ref(TIM15->CR1), TIM_CR1_CEN); // Enable counter
		stmcpp::reg::set(std::ref(TIM17->CR1), TIM_CR1_CEN); // Enable counter

		update();

	}

	void update() {
		// Update the rear strip buffer
		std::array rearPixels = rearStrip.getPixels();

		uint8_t pixelIndex = 0;
		for(Pixel p : rearPixels) {
			if (p.isOn()) {
				colorToTiming(p.getColor(), &rearBuffer[resetSlots + pixelIndex * 24]);
			} else {
				memset(&rearBuffer[resetSlots + pixelIndex * 24], zeroPeriod, 24);
			}
			
			pixelIndex++;
		}

		// Update the rear strip buffer
		std::array frontPixels = frontStrip.getPixels();

		// For each pixel, update the buffer
		pixelIndex = 0;
		for(Pixel p : frontPixels) {
			if (p.isOn()) {
				colorToTiming(p.getColor(), &frontBuffer[resetSlots + pixelIndex * 24]);
			} else {
				memset(&frontBuffer[resetSlots + pixelIndex * 24], zeroPeriod, 24);
			}
			pixelIndex++;
		}

		// Reprogram CNDTR before re-enabling — STM32G4 DMA requires this after channel was disabled
		stmcpp::reg::write(std::ref(DMA2_Channel2->CNDTR), sizeof(frontBuffer));
		stmcpp::reg::set(std::ref(DMA2_Channel2->CCR), DMA_CCR_EN);

		stmcpp::reg::write(std::ref(DMA2_Channel1->CNDTR), sizeof(rearBuffer));
		stmcpp::reg::set(std::ref(DMA2_Channel1->CCR), DMA_CCR_EN);
	}

	void colorToTiming(Color & color, uint8_t * timingBuffer){ 
		uint32_t colorRaw = color.raw();
		uint8_t r = (colorRaw >> 16) & 0xFF; // Extract red
		uint8_t g = (colorRaw >> 8) & 0xFF;  // Extract green
		uint8_t b = colorRaw & 0xFF;         // Extract blue
		
		// Fill in the 24 byte timing buffer with RGB
		uint8_t i;
		for (i = 0; i < 8; i++) 
			timingBuffer[i] = ((r << i) & 0x80) ? onePeriod : zeroPeriod;
		for (i = 0; i < 8; i++) 
			timingBuffer[8 + i] = ((g << i) & 0x80) ? onePeriod : zeroPeriod;
		for (i = 0; i < 8; i++) 
			timingBuffer[16 + i] = ((b << i) & 0x80) ? onePeriod : zeroPeriod;
	}

	extern "C" void DMA2_Channel1_IRQHandler(void) {
		if (stmcpp::reg::read(std::ref(DMA2->ISR), DMA_ISR_TCIF1)) {
			stmcpp::reg::set(std::ref(DMA2->IFCR), DMA_IFCR_CTCIF1); // Clear transfer complete flag
			stmcpp::reg::clear(std::ref(DMA2_Channel1->CCR), DMA_CCR_EN); // Enable DMA channel
		}

		NVIC_ClearPendingIRQ(DMA2_Channel1_IRQn);
	}

	extern "C" void DMA2_Channel2_IRQHandler(void) {
		if (stmcpp::reg::read(std::ref(DMA2->ISR), DMA_ISR_TCIF2)) {
			stmcpp::reg::set(std::ref(DMA2->IFCR), DMA_IFCR_CTCIF2); // Clear transfer complete flag
			stmcpp::reg::clear(std::ref(DMA2_Channel2->CCR), DMA_CCR_EN); // Enable DMA channel
		}

		NVIC_ClearPendingIRQ(DMA2_Channel2_IRQn);
	}


	void Color::rgb2hsv(uint8_t r, uint8_t g, uint8_t b, double& h, double& s, double& v) {
		double rf = r / 255.0, gf = g / 255.0, bf = b / 255.0;
		double max = std::max(std::max(rf, gf), bf);
		double min = std::min(std::min(rf, gf), bf);
		v = max;

		double delta = max - min;
		if (delta < 1e-5) {
			h = s = 0.0;
			return;
		}
		s = (max > 0.0) ? (delta / max) : 0.0;

		if (rf == max)
			h = (gf - bf) / delta;
		else if (gf == max)
			h = 2.0 + (bf - rf) / delta;
		else
			h = 4.0 + (rf - gf) / delta;

		h *= 60.0;
		if (h < 0.0) h += 360.0;
	}

	void Color::hsv2rgb(double h, double s, double v, uint8_t& r, uint8_t& g, uint8_t& b) {
		if (s <= 0.0) {
			r = g = b = static_cast<uint8_t>(v * 255.0);
			return;
		}
		h = std::fmod(h, 360.0);
		if (h < 0.0) h += 360.0;
		h /= 60.0;
		int i = static_cast<int>(h);
		double f = h - i;
		double p = v * (1.0 - s);
		double q = v * (1.0 - s * f);
		double t = v * (1.0 - s * (1.0 - f));

		double rf, gf, bf;
		switch (i) {
			case 0: rf = v; gf = t; bf = p; break;
			case 1: rf = q; gf = v; bf = p; break;
			case 2: rf = p; gf = v; bf = t; break;
			case 3: rf = p; gf = q; bf = v; break;
			case 4: rf = t; gf = p; bf = v; break;
			default: rf = v; gf = p; bf = q; break;
		}
		r = static_cast<uint8_t>(rf * 255.0);
		g = static_cast<uint8_t>(gf * 255.0);
		b = static_cast<uint8_t>(bf * 255.0);
	}

	void Color::shiftHue(int16_t degrees) {
		double h = 0, s = 0, v = 0;
		rgb2hsv(r_, g_, b_, h, s, v);
		h = std::fmod(h + degrees + 360.0, 360.0);
		hsv2rgb(h, s, v, r_, g_, b_);
	}

	void notifyActivity(uint8_t pixelIndex) {
		if (pixelIndex < 6) {
			activityTimestamp[pixelIndex] = stmcpp::systick::getDuration();
		}
	}

	void updateStatus() {
		bool master = settings::ledMasterEnable();
		bool roleId = settings::midiRoleIdentifier();
		duration now = stmcpp::systick::getDuration();

		constexpr float INTENSITY = 0.1f;
		Color off(0, 0, 0, 0);
		Color green(0, 255, 0, INTENSITY);
		Color red(255, 0, 0, INTENSITY);
		Color white(255, 255, 255, INTENSITY);

		auto &pixels = rearStrip.getPixels();

		// USB
		if (master && isPcAlive()) {
			bool blink = (now - activityTimestamp[PIXEL_USB]) < BLINK_DURATION;
			pixels[PIXEL_USB].setColorQuiet(blink ? white : green);
		} else {
			pixels[PIXEL_USB].setColorQuiet(off);
		}

		// Display
		if (master && ::display::isConnected()) {
			bool blink = (now - activityTimestamp[PIXEL_DISPLAY]) < BLINK_DURATION;
			pixels[PIXEL_DISPLAY].setColorQuiet(blink ? white : green);
		} else {
			pixels[PIXEL_DISPLAY].setColorQuiet(off);
		}

		// Current
		if (master && base::current::isEnabled()) {
			bool blink = (now - activityTimestamp[PIXEL_CURRENT]) < BLINK_DURATION;
			pixels[PIXEL_CURRENT].setColorQuiet(blink ? white : green);
		} else {
			pixels[PIXEL_CURRENT].setColorQuiet(off);
		}

		// MIDI A — vždy červená pokud roleId, bliká bíle pokud master
		if (roleId) {
			bool blink = master && (now - activityTimestamp[PIXEL_MIDIA]) < BLINK_DURATION;
			pixels[PIXEL_MIDIA].setColorQuiet(blink ? white : red);
		} else {
			pixels[PIXEL_MIDIA].setColorQuiet(off);
		}

		// MIDI B — vždy zelená pokud roleId, bliká bíle pokud master
		if (roleId) {
			bool blink = master && (now - activityTimestamp[PIXEL_MIDIB]) < BLINK_DURATION;
			pixels[PIXEL_MIDIB].setColorQuiet(blink ? white : green);
		} else {
			pixels[PIXEL_MIDIB].setColorQuiet(off);
		}

		// Bluetooth
		if (master && Bluetooth::isConnected()) {
			bool blink = (now - activityTimestamp[PIXEL_BLUETOOTH]) < BLINK_DURATION;
			pixels[PIXEL_BLUETOOTH].setColorQuiet(blink ? white : green);
		} else {
			pixels[PIXEL_BLUETOOTH].setColorQuiet(off);
		}

		update();
	}
}