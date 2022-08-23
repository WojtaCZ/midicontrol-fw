#include "display.hpp"
#include "base.hpp"
#include <stm32/gpio.h>
#include <stm32/timer.h>
#include <stm32/dma.h>
#include <stm32/dmamux.h>
#include <stm32/usart.h>
#include <stm32/rcc.h>
#include <cm3/nvic.h>
#include <stm32/exti.h>


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

extern "C" uint32_t usb_midi_tx(void *buf, int len);

namespace Display{
    //Default state - all off
    array<uint8_t, 9> state = {0xb0, 0xe0, 0xe0, 0xe0, 0xe0, 0xe0, 0xe0, 0xe0, 0xe0};
    //Receive buffer
    array<uint8_t, 9> rxBuffer;
    array<uint8_t, 9> txBuffer;
    bool connected = false;


    void init(){
        //USART RX pin
		gpio_mode_setup(GPIO::PORTA, GPIO_MODE_AF, GPIO_PUPD_NONE, GPIO::PIN10);
        gpio_set_af(GPIO::PORTA, GPIO_AF7, GPIO::PIN10);
        //USART TX pin
		gpio_mode_setup(GPIO::PORTA, GPIO_MODE_AF, GPIO_PUPD_NONE, GPIO::PIN9);
		gpio_set_af(GPIO::PORTA, GPIO_AF7, GPIO::PIN9);

        //VSENSE pin
		gpio_mode_setup(GPIO::PORTC, GPIO_MODE_INPUT, GPIO_PUPD_NONE, GPIO::PIN13);
        nvic_enable_irq(NVIC_EXTI15_10_IRQ);
        exti_select_source(EXTI13, GPIO::PORTC);
        exti_set_trigger(EXTI13, EXTI_TRIGGER_BOTH);
        exti_enable_request(EXTI13);
        
        //Check if the display wasnt aleready connected at startup
        if(gpio_get(GPIO::PORTC, GPIO::PIN13)){
            connected = false;
        }else{
            connected = true;
        }

		//DMA Receive
		dma_set_priority(DMA2, DMA_CHANNEL3, DMA_CCR_PL_MEDIUM);
		dma_set_memory_size(DMA2, DMA_CHANNEL3, DMA_CCR_MSIZE_8BIT);
		dma_set_peripheral_size(DMA2, DMA_CHANNEL3, DMA_CCR_PSIZE_8BIT);
		dma_enable_memory_increment_mode(DMA2, DMA_CHANNEL3);
        dma_enable_circular_mode(DMA2, DMA_CHANNEL3);
		dma_set_read_from_peripheral(DMA2, DMA_CHANNEL3);

		dma_enable_transfer_complete_interrupt(DMA2, DMA_CHANNEL3);
		nvic_enable_irq(NVIC_DMA2_CHANNEL3_IRQ);

		dmamux_set_dma_channel_request(DMAMUX1, DMA_CHANNEL3+8, DMAMUX_CxCR_DMAREQ_ID_UART1_RX);

		dma_set_peripheral_address(DMA2, DMA_CHANNEL3, (uint32_t)&USART1_RDR);
		dma_set_memory_address(DMA2, DMA_CHANNEL3, (uint32_t)&rxBuffer[0]);
		dma_set_number_of_data(DMA2, DMA_CHANNEL3, 9);

		//DMA Transmit
		dma_set_priority(DMA2, DMA_CHANNEL4, DMA_CCR_PL_MEDIUM);
		dma_set_memory_size(DMA2, DMA_CHANNEL4, DMA_CCR_MSIZE_8BIT);
		dma_set_peripheral_size(DMA2, DMA_CHANNEL4, DMA_CCR_PSIZE_8BIT);
		dma_enable_memory_increment_mode(DMA2, DMA_CHANNEL4);
		dma_set_read_from_memory(DMA2, DMA_CHANNEL4);

		dma_enable_transfer_complete_interrupt(DMA2, DMA_CHANNEL4);

		dmamux_set_dma_channel_request(DMAMUX1, DMA_CHANNEL4+8, DMAMUX_CxCR_DMAREQ_ID_UART1_TX);

        usart_enable_rx_timeout(USART1);
        usart_enable_rx_timeout_interrupt(USART1);
        usart_set_rx_timeout_value(USART1, 2000);
        nvic_enable_irq(NVIC_USART1_IRQ);

		usart_set_baudrate(USART1, 1200);
		usart_set_databits(USART1, 8);
		usart_set_parity(USART1, USART_PARITY_NONE);
		usart_set_stopbits(USART1, USART_STOPBITS_1);
		usart_set_flow_control(USART1, USART_FLOWCONTROL_NONE);
		usart_set_mode(USART1, USART_MODE_TX_RX);
		usart_enable_rx_dma(USART1);

		dma_enable_channel(DMA2, DMA_CHANNEL3);
		
		usart_enable(USART1);
    }

    void send(array<uint8_t, 9> data){
        if(!connected) return; 
        txBuffer = data;
        dma_disable_channel(DMA2, DMA_CHANNEL3);
        dma_set_peripheral_address(DMA2, DMA_CHANNEL4, (uint32_t)&USART1_TDR);
		dma_set_memory_address(DMA2, DMA_CHANNEL4, (uint32_t)&txBuffer[0]);
		dma_set_number_of_data(DMA2, DMA_CHANNEL4, 9);
		usart_enable_tx_dma(USART1);
		nvic_enable_irq(NVIC_DMA2_CHANNEL4_IRQ);
		dma_enable_channel(DMA2, DMA_CHANNEL4);
    }

    void setSong(uint16_t song, uint8_t visible){
        if(visible){
            if(song > 1999) song = 1999;
            state.at(3) = (song - ((uint16_t)(song/10)*10)) & 0x0f;
            state.at(4) = (song - ((uint16_t)(song/100)*100))/10 & 0x0f;
            state.at(5) = (song - ((uint16_t)(song/1000)*1000))/100 & 0x0f;
            state.at(6) = (song/1000) & 0x0f;
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
            state.at(1) = (verse - ((uint8_t)(verse/10)*10)) & 0x0f;
            state.at(2) = (verse/10) & 0x0f;
        }else{
            state.at(1) = 0xe0;
            state.at(2) = 0xe0;
        }

        send(state);
    }

    void setLetter(char letter, uint8_t visible){
        if(visible){
            if(letter > 'E' || letter < 'A') letter = 'A';
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

    array<uint8_t, 9> * getRawState(){
        return &state;
    }

    uint8_t getRawState(int index){
        if(index > state.size() || index < 0) return 0;
        return state.at(index);
    }

    extern "C" void EXTI15_10_IRQHandler(){
        exti_reset_request(EXTI13);
        if(gpio_get(GPIO::PORTC, GPIO::PIN13)){
            connected = false;
        }else{
            connected = true;
        }
    }

    extern "C" void USART1_IRQHandler(){
        //if we get an incomplete frame, throw it away
        if(dma_get_number_of_data(DMA2, DMA_CHANNEL3) != 9){
            dma_disable_channel(DMA2, DMA_CHANNEL3);
            dma_set_peripheral_address(DMA2, DMA_CHANNEL3, (uint32_t)&USART1_RDR);
            dma_set_memory_address(DMA2, DMA_CHANNEL3, (uint32_t)&rxBuffer[0]);
            dma_set_number_of_data(DMA2, DMA_CHANNEL3, 9);
            dma_enable_channel(DMA2, DMA_CHANNEL3);
        }

        //Clear the flags
        USART1_ICR |= USART_ISR_RTOF;
        nvic_clear_pending_irq(NVIC_USART1_IRQ);    
    }

    //Receive complete interrupt
    extern "C" void DMA2_Channel3_IRQHandler(){
        state = rxBuffer;
        dma_clear_interrupt_flags(DMA2, DMA_CHANNEL3, DMA_TCIF);
        nvic_clear_pending_irq(NVIC_DMA2_CHANNEL3_IRQ);
        
        //If display state changes, send out the buffer over midi as sysex
        uint8_t buff[] = {0xF0, 0x7E, state[0], state[1], state[2], state[3], state[4], state[5], state[6], state[7], state[8], 0xF7};
        usb_midi_tx((void *) buff, 12);
    }

    extern "C" void DMA2_Channel4_IRQHandler(){
        dma_disable_channel(DMA2, DMA_CHANNEL4);
        usart_disable_tx_dma(USART1);
        dma_clear_interrupt_flags(DMA2, DMA_CHANNEL4, DMA_TCIF);
        nvic_clear_pending_irq(NVIC_DMA2_CHANNEL4_IRQ);
        dma_enable_channel(DMA2, DMA_CHANNEL3);
        
    }
}

