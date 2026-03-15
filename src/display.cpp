#include "display.hpp"
#include "led.hpp"
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

        LED::notifyActivity(LED::PIXEL_DISPLAY);
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

    void setLetter(char letter, bool visible){
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

    bool isConnected(){
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

    // Převod interního stavu číslice na SysEx byte (0x00-0x09 nebo 0x0F=off)
    static uint8_t stateToSysexDigit(uint8_t state) {
        return (state & 0xF0) == 0xE0 ? 0x0F : (state & 0x0F);
    }

    // Převod interního stavu LED na SysEx byte
    static uint8_t stateToSysexLed(uint8_t state) {
        switch(state) {
            case RED:    return 0x01;
            case YELLOW: return 0x04;
            case GREEN:  return 0x02;
            case BLUE:   return 0x03;
            default:     return 0x0F; // OFF
        }
    }

    // Převod interního stavu písmene na SysEx byte
    static uint8_t stateToSysexLetter(uint8_t state) {
        uint8_t val = state & 0x0F;
        return (val >= 0x0A && val <= 0x0D) ? val : 0x0F;
    }

    // Send display state to MIDI as SysEx
    // Formát: F0 4D 43 01 00 S3 S2 S1 S0 V1 V0 LETTER LED F7
    // Všechny datové byty jsou v rozsahu 0x00-0x0F (7-bit safe)
    void sendToMIDI(){
        uint8_t s3 = stateToSysexDigit(currentState[6]); // tisíce
        uint8_t s2 = stateToSysexDigit(currentState[5]); // stovky
        uint8_t s1 = stateToSysexDigit(currentState[4]); // desítky
        uint8_t s0 = stateToSysexDigit(currentState[3]); // jednotky
        uint8_t v1 = stateToSysexDigit(currentState[2]); // desítky sloky
        uint8_t v0 = stateToSysexDigit(currentState[1]); // jednotky sloky
        uint8_t letter = stateToSysexLetter(currentState[7]);
        uint8_t led = stateToSysexLed(currentState[8]);

        // USB MIDI packets (4 bytes each: [CIN, MIDI1, MIDI2, MIDI3])
        const uint8_t packets[][4] = {
            {0x04, 0xF0, 0x4D, 0x43},     // SysEx start + header
            {0x04, 0x01, 0x00, s3},        // device, target, S3
            {0x04, s2,   s1,   s0},        // S2, S1, S0
            {0x04, v1,   v0,   letter},    // V1, V0, LETTER
            {0x06, led,  0xF7, 0x00},      // LED, SysEx end (CIN=6: 2-byte end)
        };

        for(const auto& pkt : packets) {
            tud_midi_packet_write(pkt);
        }
    }

    extern "C" void EXTI15_10_IRQHandler(){
        display::disp_sense.clearInterruptFlag(); // Clear the interrupt flag
        display::connected = !disp_sense.read(); // Update the connected state based on the pin state
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
