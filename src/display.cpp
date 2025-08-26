#include "display.hpp"
//#include "base.hpp"
//#include "scheduler.hpp"
#include <stm32g431xx.h>
#include <core_cm4.h>
#include <cstdint>
#include <cmsis_compiler.h>
#include <tinyusb/src/device/usbd.h>
#include <tinyusb/src/class/midi/midi_device.h>

#include "stmcpp/register.hpp"
#include "stmcpp/gpio.hpp"

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



//Scheduler dispChangeScheduler(300, [](){ if(Display::wasChanged() && Display::getConnected()) Display::send();}, Scheduler::DISPATCH_ON_INCREMENT | Scheduler::PERIODICAL | Scheduler::ACTIVE);


namespace display {

    //Default state - all off
    uint8_t currentState[] = {0xb0, 0xe0, 0xe0, 0xe0, 0xe0, 0xe0, 0xe0, 0x0e, 0xe0};
    //Receive buffer
    uint8_t rxBuffer[9];
    uint8_t txBuffer[9];
    bool connected = false;
    bool changed = true;

    static stmcpp::gpio::pin<stmcpp::gpio::port::portc, 13> disp_sense (stmcpp::gpio::mode::input);


    void init(){

        // USART GPIOs
		stmcpp::gpio::pin<stmcpp::gpio::port::porta, 9> usart_tx (stmcpp::gpio::mode::af7, stmcpp::gpio::otype::pushPull, stmcpp::gpio::speed::low, stmcpp::gpio::pull::noPull);
		stmcpp::gpio::pin<stmcpp::gpio::port::porta, 10> usart_rx (stmcpp::gpio::mode::af7);
        disp_sense.enableInterrupt(stmcpp::gpio::interrupt::edge::both); // Sense pin for display connection
        NVIC_EnableIRQ(EXTI15_10_IRQn);

        // Check if the display wasn't already connected at startup
        connected = !disp_sense.read();


        stmcpp::reg::write(std::ref(DMA2_Channel3->CCR), 
            (0b1 << DMA_CCR_MINC_Pos) | // Memory increment mode
            (0b1 << DMA_CCR_CIRC_Pos) | // Circular DMA
            (0b1 << DMA_CCR_TCIE_Pos)   // Transfer complete interrupt enable
            
        );

        // Set up the DMA memory and peripheral
        stmcpp::reg::write(std::ref(DMA2_Channel3->CMAR), reinterpret_cast<uint32_t>(&rxBuffer[0]));
        stmcpp::reg::write(std::ref(DMA2_Channel3->CPAR), reinterpret_cast<uint32_t>(&USART1->RDR));
        stmcpp::reg::write(std::ref(DMA2_Channel3->CNDTR), sizeof(rxBuffer));
        NVIC_EnableIRQ(DMA2_Channel3_IRQn);

        // Set up DMAMUX routing (DMAMUX channels map to 6-11 to DMA2 1-6, thats why the offset in numbering)
        stmcpp::reg::write(std::ref(DMAMUX1_Channel8->CCR), (24 << DMAMUX_CxCR_DMAREQ_ID_Pos));


        stmcpp::reg::write(std::ref(DMA2_Channel4->CCR), 
            (0b1 << DMA_CCR_MINC_Pos) | // Memory increment mode
            (0b1 << DMA_CCR_DIR_Pos) | // Memory to peripheral
            (0b1 << DMA_CCR_TCIE_Pos)   // Transfer complete interrupt enable
        );

        // Set up the DMA memory and peripheral
        stmcpp::reg::write(std::ref(DMA2_Channel4->CMAR), reinterpret_cast<uint32_t>(&txBuffer[0]));
        stmcpp::reg::write(std::ref(DMA2_Channel4->CPAR), reinterpret_cast<uint32_t>(&USART1->TDR));
        stmcpp::reg::write(std::ref(DMA2_Channel4->CNDTR), sizeof(txBuffer));
        NVIC_EnableIRQ(DMA2_Channel4_IRQn);

        // Set up DMAMUX routing (DMAMUX channels map to 6-11 to DMA2 1-6, thats why the offset in numbering)
        stmcpp::reg::write(std::ref(DMAMUX1_Channel9->CCR), (25 << DMAMUX_CxCR_DMAREQ_ID_Pos));

        // Set up USART1
		stmcpp::reg::write(std::ref(USART1->BRR), 16000000 / 1200); 
		stmcpp::reg::write(std::ref(USART1->CR1), 
			USART_CR1_TE 		| // Transmitter enable
			USART_CR1_RE 		| // Receiver enable
			USART_CR1_RTOIE 	  // Receiver timeout enable
		);

		stmcpp::reg::write(std::ref(USART1->CR3), 
			USART_CR3_DMAR | // DMA enable for reception
            USART_CR3_DMAT   // DMA enable for transmission
		);

        stmcpp::reg::write(std::ref(USART1->RTOR), 100); // Set receiver timeout to 100 bits

		// Enable USART1 IRQ
        NVIC_EnableIRQ(USART1_IRQn);

        // Enable USART1
        stmcpp::reg::set(std::ref(USART1->CR1), USART_CR1_UE); 

        // Enable the RX DMA
        stmcpp::reg::set(std::ref(DMA2_Channel3->CCR), DMA_CCR_EN); 
    }

    void sendState(){
        if(!connected) return; 

        memcpy(txBuffer, currentState, sizeof(txBuffer));
        // Send display state over MIDI if changed
        sendToMIDI();

        // Disable the receiving DMA
        stmcpp::reg::clear(std::ref(DMA2_Channel3->CCR), DMA_CCR_EN); 

        // Reconfigure the TX DMA
        stmcpp::reg::clear(std::ref(DMA2_Channel4->CCR), DMA_CCR_EN); 
        stmcpp::reg::write(std::ref(DMA2_Channel4->CMAR), reinterpret_cast<uint32_t>(&txBuffer[0]));
        stmcpp::reg::write(std::ref(DMA2_Channel4->CNDTR), sizeof(txBuffer));
        stmcpp::reg::set(std::ref(DMA2_Channel4->CCR), DMA_CCR_EN); 

        changed = false;
    }

    void setSong(uint16_t song, bool visible){
        if(visible){
            if(song > 1999) song = 1999;
            currentState[3] = (song % 10) & 0x0f;
            currentState[4] = (song / 10 % 10) & 0x0f;
            currentState[5] = (song / 100 % 10) & 0x0f;
            currentState[6] = (song / 1000) & 0x0f;
        }else{
            currentState[3] = 0xe0;
            currentState[4] = 0xe0;
            currentState[5] = 0xe0;
            currentState[6] = 0xe0;
        }

        sendState();
    }

    void setVerse(uint8_t verse, bool visible){
        if(visible){
            if(verse > 99) verse = 99;
            currentState[1] = (verse % 10) & 0x0f;
            currentState[2] = (verse / 10) & 0x0f;
        }else{
            currentState[1] = 0xe0;
            currentState[2] = 0xe0;
        }

        sendState();
    }

    void setLetter(char letter, uint8_t visible){
        if(visible){
            if(letter > 'D' || letter < 'A') letter = 'A';
            currentState[7] = 0x0f & (letter - 55);
        }else{
            currentState[7] = 0xe0;
        }

        sendState();
    }

    void setLed(ledColor led){
        currentState[8] = led;
        sendState();
    }

    uint16_t getSong(){
        return (currentState[3] & 0x0f) + (currentState[4] & 0x0f)*10 + (currentState[5] & 0x0f)*100 + (currentState[6] & 0x0f)*1000;
    }

    uint8_t getVerse(){
        return (currentState[1] & 0x0f) + (currentState[2] & 0x0f)*10;
    }

    char getLetter(){
        return (currentState[7] & 0x0f) + 55;
    }

    ledColor getLed(){
        return (ledColor)(currentState[8]);
    }

    bool getConnected(){
        return connected;
    }

    bool wasChanged(){
        return changed;
    }

    uint8_t getRawState(uint8_t index){
        if(index >= sizeof(currentState) || index < 0) return 0;
        return currentState[index];
    }

    void setRawState(uint8_t data, uint8_t index){
        if(index >= sizeof(currentState) || index < 0) return;
        currentState[index] = data;
        changed = true;
    }

    // Send display state to MIDI as sysex
    void sendToMIDI(){
       /* // Create a MIDI message
        const uint8_t buff[] = {
            // Header - sysex start + real-time + ID (assigned 1 to display)
            0x04, 0xF0, 0x7E, 0x01, 
            0x04, Display::getRawSysex(1), Display::getRawSysex(2), Display::getRawSysex(3),
            0x04, Display::getRawSysex(4), Display::getRawSysex(5), Display::getRawSysex(6),
            0x07, Display::getRawSysex(7), Display::getRawSysex(8), 0xF7
        };

        // Send the message over
        tud_midi_packet_write(&buff[0]);*/
    }

    // Returns the raw state bytes formatted for midi sysex message
    /*uint8_t getRawSysex(int index){
        if(index > state.size() || index < 0) return 0;
        // Sysex doesn't allow high bits set in data- reformat 0xE0 to 0x0E
        if(state.at(index) > 0x0F && index != 8) return (getRawState(index) >> 4) & 0x0F;
        // Handle LED byte which needs a format of 0x2X
        if(index == 8 && (state.at(index) & 0xF0) == 0xE0) return 0x0E;

        return state.at(index) & 0x0F;
    }
*/



    /*extern "C" void display_setRawSysex(uint8_t data, int index){
        // Reinterpret invalid data as digit off
        if(data >= 0x0E && index != 8) data = 0xE0;
        // Handle LED byte with specific format
        if(data > 3 && index == 8) data = 0xE0;
        if(data <= 3 && index == 8) data |= 0x20;

        setRawState(data, index);
    }*/

    extern "C" void EXTI15_10_IRQHandler(){
        display::disp_sense.clearInterruptFlag(); // Clear the interrupt flag
        display::connected = !display::disp_sense.read(); // Update the connected state based on the pin state
        NVIC_ClearPendingIRQ(EXTI15_10_IRQn);  
    }

    extern "C" void USART1_IRQHandler(){
        // If we get an incomplete frame, throw it away
        if(stmcpp::reg::read(std::ref(DMA2_Channel3->CNDTR)) != 9){
            stmcpp::reg::clear(std::ref(DMA2_Channel3->CCR), DMA_CCR_EN); 
            stmcpp::reg::write(std::ref(DMA2_Channel3->CNDTR), sizeof(display::rxBuffer));
            stmcpp::reg::set(std::ref(DMA2_Channel3->CCR), DMA_CCR_EN); 
        }

        // Clear the timeout flag
        stmcpp::reg::set(std::ref(USART1->ICR), USART_ICR_RTOCF);
        NVIC_ClearPendingIRQ(USART1_IRQn);    
    }

    // Receive complete interrupt
    extern "C" void DMA2_Channel3_IRQHandler(){
        if(stmcpp::reg::read(std::ref(DMA2->ISR)) & DMA_ISR_TCIF3){
            stmcpp::reg::set(std::ref(DMA2->IFCR), DMA_IFCR_CTCIF3); 
            memcpy(display::currentState, display::rxBuffer, sizeof(display::rxBuffer)); // Copy the received data to the current state

        }

        NVIC_ClearPendingIRQ(DMA2_Channel3_IRQn);
        
        // Send display state over MIDI if changed
        sendToMIDI();
    }

    // Transmit complete interrupt
    extern "C" void DMA2_Channel4_IRQHandler(){
        if(stmcpp::reg::read(std::ref(DMA2->ISR)) & DMA_ISR_TCIF4){
            stmcpp::reg::set(std::ref(DMA2->IFCR), DMA_IFCR_CTCIF4); 
            // Disable the DMA channel after transmission
            stmcpp::reg::clear(std::ref(DMA2_Channel4->CCR), DMA_CCR_EN); 

            // Turn the RX DMA back on
            stmcpp::reg::set(std::ref(DMA2_Channel3->CCR), DMA_CCR_EN); 
            display::changed = false;
           
        }

        NVIC_ClearPendingIRQ(DMA2_Channel4_IRQn);
    }
}
