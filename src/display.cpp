#include "display.hpp"
#include "base.hpp"
#include "scheduler.hpp"
#include <stm32g431xx.h>
#include <core_cm4.h>
#include <cmsis_compiler.h>
#include <tinyusb/src/device/usbd.h>
#include <tinyusb/src/class/midi/midi_device.h>

/* PROTOCOL NOTE
Display expects 9byte frames over inverted USART. In each byte, 0xe in MSW (most significant word) means the segment is turned off. Frame structure follows:
0: Flag     - always equal 0xb0
1: Verse    - singles
2: Verse    - dozens
3: Song     - singles
4: Song     - dozens
5: Song     - hundrets
6: Song     - thousands
7: Letter   - A to D represented in HEX 
8: LED      - 0 to 3 representing R, Y, G, B
*/

using namespace std;

Scheduler dispChangeScheduler(300, [](){ if(Display::wasChanged() && Display::getConnected()) Display::send();}, Scheduler::DISPATCH_ON_INCREMENT | Scheduler::PERIODICAL | Scheduler::ACTIVE);

namespace Display{
    //Default state - all off
    array<uint8_t, 9> state = {0xb0, 0xe0, 0xe0, 0xe0, 0xe0, 0xe0, 0xe0, 0x0e, 0xe0};
    //Receive buffer
    array<uint8_t, 9> rxBuffer;
    array<uint8_t, 9> txBuffer;
    bool connected = false;
    bool changed = true;

    void init(){
        // Enable GPIOA and GPIOC clocks
        RCC->AHB2ENR |= RCC_AHB2ENR_GPIOAEN | RCC_AHB2ENR_GPIOCEN;
        // Enable USART1 clock
        RCC->APB2ENR |= RCC_APB2ENR_USART1EN;
        // Enable DMA2 clock
        RCC->AHB1ENR |= RCC_AHB1ENR_DMA2EN;

        // USART RX pin (PA10)
        GPIOA->MODER = (GPIOA->MODER & ~(GPIO_MODER_MODE10_Msk)) | (GPIO_MODER_MODE10_1);
        GPIOA->AFR[1] = (GPIOA->AFR[1] & ~(GPIO_AFRH_AFSEL10_Msk)) | (7 << GPIO_AFRH_AFSEL10_Pos);
        // USART TX pin (PA9)
        GPIOA->MODER = (GPIOA->MODER & ~(GPIO_MODER_MODE9_Msk)) | (GPIO_MODER_MODE9_1);
        GPIOA->AFR[1] = (GPIOA->AFR[1] & ~(GPIO_AFRH_AFSEL9_Msk)) | (7 << GPIO_AFRH_AFSEL9_Pos);

        // VSENSE pin (PC13)
        GPIOC->MODER &= ~(GPIO_MODER_MODE13_Msk);
        EXTI->IMR1 |= EXTI_IMR1_IM13;
        EXTI->RTSR1 |= EXTI_RTSR1_RT13;
        EXTI->FTSR1 |= EXTI_FTSR1_FT13;
        NVIC_EnableIRQ(EXTI15_10_IRQn);

        // Check if the display wasn't already connected at startup
        connected = !(GPIOC->IDR & GPIO_IDR_ID13);

        // DMA Receive
        DMA2_Channel3->CCR = DMA_CCR_PL_0 | DMA_CCR_MINC | DMA_CCR_CIRC | DMA_CCR_TCIE;
        DMA2_Channel3->CNDTR = 9;
        DMA2_Channel3->CPAR = (uint32_t)&USART1->RDR;
        DMA2_Channel3->CMAR = (uint32_t)rxBuffer.data();
        NVIC_EnableIRQ(DMA2_Channel3_IRQn);

        // DMA Transmit
        DMA2_Channel4->CCR = DMA_CCR_PL_0 | DMA_CCR_MINC | DMA_CCR_DIR | DMA_CCR_TCIE;
        DMA2_Channel4->CNDTR = 9;
        DMA2_Channel4->CPAR = (uint32_t)&USART1->TDR;
        DMA2_Channel4->CMAR = (uint32_t)txBuffer.data();
        NVIC_EnableIRQ(DMA2_Channel4_IRQn);

        // USART configuration
        USART1->BRR = 80000000 / 1200; // Assuming 80MHz clock
        USART1->CR1 = USART_CR1_TE | USART_CR1_RE | USART_CR1_UE | USART_CR1_RTOIE;
        USART1->CR2 = 0;
        USART1->CR3 = USART_CR3_DMAR | USART_CR3_DMAT;
        USART1->RTOR = 2000;
        NVIC_EnableIRQ(USART1_IRQn);

        DMA2_Channel3->CCR |= DMA_CCR_EN;
    }

    void send(array<uint8_t, 9> data){
        if(!connected) return; 
        txBuffer = data;
        // Send display state over MIDI if changed
        sendToMIDI();
        // Send data over USART
        DMA2_Channel4->CCR &= ~DMA_CCR_EN;
        DMA2_Channel4->CNDTR = 9;
        DMA2_Channel4->CCR |= DMA_CCR_EN;
        changed = false;
    }

    void send(){
        send(state);
    }

    void setSong(uint16_t song, uint8_t visible){
        if(visible){
            if(song > 1999) song = 1999;
            state.at(3) = (song % 10) & 0x0f;
            state.at(4) = (song / 10 % 10) & 0x0f;
            state.at(5) = (song / 100 % 10) & 0x0f;
            state.at(6) = (song / 1000) & 0x0f;
        }else{
            state.at(3) = 0xe0;
            state.at(4) = 0xe0;
            state.at(5) = 0xe0;
            state.at(6) = 0xe0;
        }

        send(state);
    }

    void setVerse(uint8_t verse, uint8_t visible){
        if(visible){
            if(verse > 99) verse = 99;
            state.at(1) = (verse % 10) & 0x0f;
            state.at(2) = (verse / 10) & 0x0f;
        }else{
            state.at(1) = 0xe0;
            state.at(2) = 0xe0;
        }

        send(state);
    }

    void setLetter(char letter, uint8_t visible){
        if(visible){
            if(letter > 'D' || letter < 'A') letter = 'A';
            state.at(7) = 0x0f & (letter - 55);
        }else{
            state.at(7) = 0xe0;
        }

        send(state);
    }

    void setLed(LED led){
        state.at(8) = led;
        send(state);
    }

    uint16_t getSong(){
        return (state.at(3) & 0x0f) + (state.at(4) & 0x0f)*10 + (state.at(5) & 0x0f)*100 + (state.at(6) & 0x0f)*1000;
    }

    uint8_t getVerse(){
        return (state.at(1) & 0x0f) + (state.at(2) & 0x0f)*10;
    }

    char getLetter(){
        return (state.at(7) & 0x0f) + 55;
    }

    LED getLed(){
        return (LED)(state.at(8));
    }

    bool getConnected(){
        return connected;
    }

    bool wasChanged(){
        return changed;
    }

    array<uint8_t, 9> * getRawState(){
        return &state;
    }

    uint8_t getRawState(int index){
        if(index > state.size() || index < 0) return 0;
        return state.at(index);
    }

    // Send display state to MIDI as sysex
    void sendToMIDI(){
        // Create a MIDI message
        const uint8_t buff[] = {
            // Header - sysex start + real-time + ID (assigned 1 to display)
            0x04, 0xF0, 0x7E, 0x01, 
            0x04, Display::getRawSysex(1), Display::getRawSysex(2), Display::getRawSysex(3),
            0x04, Display::getRawSysex(4), Display::getRawSysex(5), Display::getRawSysex(6),
            0x07, Display::getRawSysex(7), Display::getRawSysex(8), 0xF7
        };

        // Send the message over
        tud_midi_packet_write(&buff[0]);
    }

    // Returns the raw state bytes formatted for midi sysex message
    uint8_t getRawSysex(int index){
        if(index > state.size() || index < 0) return 0;
        // Sysex doesn't allow high bits set in data- reformat 0xE0 to 0x0E
        if(state.at(index) > 0x0F && index != 8) return (getRawState(index) >> 4) & 0x0F;
        // Handle LED byte which needs a format of 0x2X
        if(index == 8 && (state.at(index) & 0xF0) == 0xE0) return 0x0E;

        return state.at(index) & 0x0F;
    }

    void setRawState(array<uint8_t, 9> state){
        int index = 0;
        for(uint8_t byte : state){
            setRawState(byte, index);
            index++;
        }
    }

    void setRawState(uint8_t data, int index){
        if(index > state.size() || index < 0) return;
        state.at(index) = data;
        changed = true;
    }

    extern "C" void display_setRawSysex(uint8_t data, int index){
        // Reinterpret invalid data as digit off
        if(data >= 0x0E && index != 8) data = 0xE0;
        // Handle LED byte with specific format
        if(data > 3 && index == 8) data = 0xE0;
        if(data <= 3 && index == 8) data |= 0x20;

        setRawState(data, index);
    }

    extern "C" void EXTI15_10_IRQHandler(){
        EXTI->PR1 = EXTI_PR1_PIF13;
        connected = !(GPIOC->IDR & GPIO_IDR_ID13);
    }

    extern "C" void USART1_IRQHandler(){
        // If we get an incomplete frame, throw it away
        if(DMA2_Channel3->CNDTR != 9){
            DMA2_Channel3->CCR &= ~DMA_CCR_EN;
            DMA2_Channel3->CNDTR = 9;
            DMA2_Channel3->CCR |= DMA_CCR_EN;
        }

        USART1->ICR = USART_ICR_RTOCF;
        NVIC_ClearPendingIRQ(USART1_IRQn);    
    }

    // Receive complete interrupt
    extern "C" void DMA2_Channel3_IRQHandler(){
        state = rxBuffer;
        DMA2->IFCR = DMA_IFCR_CTCIF3;
        NVIC_ClearPendingIRQ(DMA2_Channel3_IRQn);
        
        // Send display state over MIDI if changed
        sendToMIDI();
    }

    // Transmit complete interrupt
    extern "C" void DMA2_Channel4_IRQHandler(){
        DMA2_Channel4->CCR &= ~DMA_CCR_EN;
        USART1->CR3 &= ~USART_CR3_DMAT;
        DMA2->IFCR = DMA_IFCR_CTCIF4;
        NVIC_ClearPendingIRQ(DMA2_Channel4_IRQn);
        DMA2_Channel3->CCR |= DMA_CCR_EN;
        changed = false;
    }
}
