
#ifndef BASE_H
#define BASE_H

#include <stm32/gpio.h>
#include <cm3/nvic.h>
#include "scheduler.hpp"
#include <string>

namespace Base{
    using namespace std;
    static string DEVICE_NAME = "MIDIControl";
    static string DEVICE_TYPE = "BASE";
    static string FW_VERSION = "2.0.0";

    void init(void);

    namespace CurrentSource{
            bool isEnabled();
            void enable();
            void disable();
            void toggle();
    };

    namespace Encoder{
            void process();
    };

}

namespace Gpio{

constexpr auto PORT_FRONT_LED = GPIOB;
constexpr auto GPIO_FRONT_LED = GPIO14;

constexpr auto PORT_BACK_LED = GPIOB;
constexpr auto GPIO_BACK_LED = GPIO5;

constexpr auto PORT_USB_DP = GPIOA;
constexpr auto GPIO_USB_DP = GPIO12;

constexpr auto PORT_USB_DM = GPIOA;
constexpr auto GPIO_USB_DM = GPIO11;

constexpr auto PORT_I2C1_SDA = GPIOB;
constexpr auto GPIO_I2C1_SDA = GPIO7;

constexpr auto PORT_I2C1_SCL = GPIOA;
constexpr auto GPIO_I2C1_SCL = GPIO15;

constexpr auto PORT_USART_MIDI_TX = GPIOB;
constexpr auto GPIO_USART_MIDI_TX = GPIO10;

constexpr auto PORT_USART_MIDI_RX = GPIOB;
constexpr auto GPIO_USART_MIDI_RX = GPIO11;

constexpr auto PORT_USART_BLE_RX = GPIOA;
constexpr auto GPIO_USART_BLE_RX = GPIO3;

constexpr auto PORT_USART_BLE_TX = GPIOA;
constexpr auto GPIO_USART_BLE_TX = GPIO2;

constexpr auto PORT_BLE_MODE = GPIOA;
constexpr auto GPIO_BLE_MODE = GPIO0;

constexpr auto PORT_BLE_RST = GPIOA;
constexpr auto GPIO_BLE_RST = GPIO1;




constexpr auto PORT_ENCODER = GPIOB;
constexpr auto GPIO_ENCODER_A = GPIO0;
constexpr auto GPIO_ENCODER_A_IRQ = NVIC_EXTI0_IRQ;
constexpr auto GPIO_ENCODER_B = GPIO2;
constexpr auto GPIO_ENCODER_B_IRQ = NVIC_EXTI2_IRQ;
constexpr auto GPIO_ENCODER_SW = GPIO1;
constexpr auto GPIO_ENCODER_SW_IRQ = NVIC_EXTI1_IRQ;

constexpr auto PORT_CURRENT_SOURCE = GPIOA;
constexpr auto GPIO_CURRENT_SOURCE = GPIO4;

}


#endif
