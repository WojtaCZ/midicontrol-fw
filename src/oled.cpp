#include "oled.hpp"
#include "scheduler.hpp"
#include "base.hpp"
#include "menu.hpp"

#include <libopencm3/stm32/timer.h>
#include <libopencm3/stm32/dma.h>
#include <libopencm3/stm32/dmamux.h>
#include <libopencm3/stm32/i2c.h>
#include <libopencm3/stm32/rcc.h>
#include <libopencm3/cm3/nvic.h>

//Scheduler used to time oled sleep
Scheduler oledSleepScheduler(OLED_SLEEP_INTERVAL, &Oled::sleepCallback, Scheduler::ACTIVE | Scheduler::DISPATCH_ON_INCREMENT);

namespace Oled{
    //Coordinates on the oled
    pair<uint16_t, uint16_t> coordinates;
    //Flags
    bool inverted;
    bool initialized;
    bool sleeping;
    //Screen buffer storing image data
    array<uint8_t, OLED_SCREENBUF_SIZE> screenBuffer;
    //Buffers used for initialization
    array<uint8_t, 4> pageBuffer;

    const array<uint8_t, 29> initBuffer = {
        //CMD type
        OLED_MEM_CMD, 
        //Display Off
        0xae,
        //Memory addressing mode
        0x20, 
        //00,Horizontal Addressing Mode;
        //01,Vertical Addressing Mode;
        //10,Page Addressing Mode (RESET); 
        0x10, 
        //Set page start address
        0xb0, 
        //COM Out scan direction
        0xc8, 
        //Low column address
        0x00, 
        //High column address
        0x10, 
        //Start line address
        0x40, 
        //Contrast
        0x81, 
        0xff, 
        //Segment remap 0 to 127
        0xa1, 
        //Normal color
        0xa6, 
        //Set MUX ratio
        0xa8, 
        0x3f,
        //Output follows RAM 
        0xa4, 
        //Set oled offset
        0xd3, 
        0x00,
        //Set oled clock divide ratio 
        0xd5, 
        0xf0, 
        //Set precharge period
        0xd9, 
        0x34,
        //Set COM pin HW config 
        0xda, 
        0x12, 
        //Set VCOMH (0.77*Vcc)
        0xdb, 
        0x20, 
        //Set DC-DC enable
        0x8d, 
        0x14, 
        //Turn on oled
        0xaf
    };

    //DMA flags
    uint8_t dmaStatus;
    uint16_t dmaIndex;

    //Wakeup function for the oled
    void wakeup(){
        if(isSleeping()){
            sleeping = false;
        }
    }

    //Sleep function for the oled
    void sleep(){
        if(!isSleeping()){
            fill(Color::BLACK);
            update();
            sleeping = true;
        }
    }

    bool isSleeping(){
        return sleeping;
    }

    void setCoordinates(pair<uint16_t, uint16_t> coord){
        coordinates = coord;
    }

    bool isInitialized(){
        return initialized;
    }

    void setInitialized(bool state){
        initialized = state;
    }

    bool isInverted(){
        return inverted;
    }

    void init(){

        //Setup the GPIO for I2C
        gpio_mode_setup(GPIO::PORTA, GPIO_MODE_AF, GPIO_PUPD_PULLUP, GPIO::PIN15);
        gpio_mode_setup(GPIO::PORTB, GPIO_MODE_AF, GPIO_PUPD_PULLUP, GPIO::PIN7);
        gpio_set_output_options(GPIO::PORTA, GPIO_OTYPE_OD, GPIO_OSPEED_100MHZ, GPIO::PIN15);
        gpio_set_output_options(GPIO::PORTB, GPIO_OTYPE_OD, GPIO_OSPEED_100MHZ, GPIO::PIN7);
        gpio_set_af(GPIO::PORTA, GPIO_AF4, GPIO::PIN15);
        gpio_set_af(GPIO::PORTB, GPIO_AF4, GPIO::PIN7);

        //Setup DMA channel
        dma_set_priority(DMA1, DMA_CHANNEL3, DMA_CCR_PL_HIGH);
        dma_set_memory_size(DMA1, DMA_CHANNEL3, DMA_CCR_MSIZE_8BIT);
        dma_set_peripheral_size(DMA1, DMA_CHANNEL3, DMA_CCR_PSIZE_8BIT);
        dma_enable_memory_increment_mode(DMA1, DMA_CHANNEL3);
        dma_set_read_from_memory(DMA1, DMA_CHANNEL3);

        //Enable interrupts
        dma_enable_transfer_complete_interrupt(DMA1, DMA_CHANNEL3);
        //nvic_set_priority(NVIC_DMA1_CHANNEL3_IRQ, );
        nvic_enable_irq(NVIC_DMA1_CHANNEL3_IRQ);

        dmamux_set_dma_channel_request(DMAMUX1, DMA_CHANNEL3, DMAMUX_CxCR_DMAREQ_ID_I2C1_TX);

        //Configure transmit register
        dma_set_peripheral_address(DMA1, DMA_CHANNEL3, (uint32_t)&I2C1_TXDR);

        i2c_peripheral_disable(I2C1);

        //Setup filtering
        i2c_enable_analog_filter(I2C1);
        i2c_set_digital_filter(I2C1, 0);

        //Setup I2C clocks
        i2c_set_prescaler(I2C1, 5);
        i2c_set_scl_low_period(I2C1, 14);
        i2c_set_scl_high_period(I2C1, 3);
        i2c_set_data_hold_time(I2C1, 1);
        i2c_set_data_setup_time(I2C1, 1);
        i2c_enable_stretching(I2C1);

        //Setup adresses
        i2c_set_7bit_addr_mode(I2C1);
        i2c_peripheral_enable(I2C1);
        i2c_set_7bit_address(I2C1, OLED_ADD);
        
        //Setup the DMA channel to get data from the prepared buffer
        dma_set_memory_address(DMA1, DMA_CHANNEL3, reinterpret_cast<uint32_t> (&initBuffer[0]));
        dma_set_number_of_data(DMA1, DMA_CHANNEL3, 29);
        i2c_set_bytes_to_transfer(I2C1, 29);
        //i2c_enable_interrupt(I2C1, I2C_CR1_TCIE);
        i2c_enable_autoend(I2C1);

        dma_enable_channel(DMA1, DMA_CHANNEL3);

        //Enable DMA and start sending data
        i2c_enable_txdma(I2C1);
        i2c_send_start(I2C1);
    }

    void update(){
        if(isInitialized() && !isSleeping()){
            //Prepare the buffer to contain frame data
            pageBuffer.at(0) = OLED_MEM_CMD;
            pageBuffer.at(1) = 0xB0;
            pageBuffer.at(2) = 0x00;
            pageBuffer.at(3) = 0x10;

            screenBuffer.at(0)         = OLED_MEM_DAT;
            screenBuffer.at(131)       = OLED_MEM_DAT;
            screenBuffer.at(262)       = OLED_MEM_DAT;
            screenBuffer.at(393)       = OLED_MEM_DAT;
            screenBuffer.at(524)       = OLED_MEM_DAT;
            screenBuffer.at(655)       = OLED_MEM_DAT;
            screenBuffer.at(786)       = OLED_MEM_DAT;
            screenBuffer.at(917)       = OLED_MEM_DAT;

            dmaStatus = 0;
            dmaIndex = 0;

            //Setup DMA to point to this buffer correctly
            dma_set_memory_address(DMA1, DMA_CHANNEL3, reinterpret_cast<uint32_t> (&pageBuffer[0]));
            dma_set_number_of_data(DMA1, DMA_CHANNEL3, 4);
            i2c_set_bytes_to_transfer(I2C1, 4);
            //i2c_enable_interrupt(I2C1, I2C_CR1_TCIE);
            i2c_enable_autoend(I2C1);

            //Enable DMA
            dma_enable_channel(DMA1, DMA_CHANNEL3);

            //Start I2C DMA transfer
            i2c_enable_txdma(I2C1);
            i2c_send_start(I2C1);
        }

    }

    extern "C" void DMA1_Channel3_IRQHandler(void){
        //Disable DMA so it doesnt fire while in interrupt
        dma_disable_channel(DMA1, DMA_CHANNEL3);
        dma_clear_interrupt_flags(DMA1, DMA_CHANNEL3, DMA_TCIF);
        nvic_clear_pending_irq(NVIC_DMA1_CHANNEL3_IRQ);

        //Check if the init sequence has been done
        if(isInitialized()){
            if(dmaStatus < 15){
                if(dmaStatus % 2){
                    pageBuffer.at(1)++;
                    dmaIndex += 131;
                    dma_set_memory_address(DMA1, DMA_CHANNEL3, reinterpret_cast<uint32_t> (&pageBuffer[0]));
                    dma_set_number_of_data(DMA1, DMA_CHANNEL3, 4);
                    i2c_set_bytes_to_transfer(I2C1, 4);
                    dma_enable_channel(DMA1, DMA_CHANNEL3);
                }else{
                    dma_set_memory_address(DMA1, DMA_CHANNEL3, reinterpret_cast<uint32_t> (&screenBuffer[dmaIndex]));
                    dma_set_number_of_data(DMA1, DMA_CHANNEL3, 131);
                    i2c_set_bytes_to_transfer(I2C1, 131);
                    dma_enable_channel(DMA1, DMA_CHANNEL3);
                }
                dmaStatus++;
                i2c_enable_autoend(I2C1);
                i2c_send_start(I2C1);
            }else{
                dma_disable_channel(DMA1, DMA_CHANNEL3);
            }
        }else{
            dma_disable_channel(DMA1, DMA_CHANNEL3);
            i2c_disable_txdma(I2C1);
            setInitialized(true);
        }

        dma_clear_interrupt_flags(DMA1, DMA_CHANNEL3, DMA_TCIF);
        nvic_clear_pending_irq(NVIC_DMA1_CHANNEL3_IRQ);
    }

    //Callback for the sleep scheduler
    void sleepCallback(void){
        oledSleepScheduler.pause();
        sleep();
    }

    //Callback to wakeup the oled
    void wakeupCallback(void){
        oledSleepScheduler.reset();
        oledSleepScheduler.resume();
        wakeup();
    }

    //Fill the whole screen with the given color
    void fill(Color color) {
        for(uint32_t i = 0; i < screenBuffer.size(); i++) {
            if(i % 131){
                screenBuffer.at(i) = (color == Color::BLACK) ? 0x00 : 0xFF;
            }else continue;
        }
    }


    //Draw a pixel in the screenbuffer
    void drawPixel(pair<uint16_t, uint16_t> coord, Color color) {

        coord.first += OLED_XOFFSET;
        coord.second += OLED_YOFFSET;
        
        if(coord.first >= OLED_WIDTH || coord.second >= OLED_HEIGHT) {
            // Don't write outside the buffer
            return;
        }

        // Check if pixel should be inverted
        if(isInverted()) {
        color = !color;
        }

        // Draw in the right color
        if(color == Color::WHITE) {
            screenBuffer.at(1 + (coord.second/8) + coord.first + (coord.second / 8) * OLED_WIDTH) |= 1 << (coord.second % 8);
        } else {
            screenBuffer.at(1 + (coord.second/8)+ coord.first + (coord.second / 8) * OLED_WIDTH) &= ~(1 << (coord.second % 8));
        }
    }

    //Draw a char to the screen buffer
    void writeSymbol(char c, FontDef font, Color color) {
        uint32_t i, b, j;

        // Check remaining space on current line
        if (OLED_WIDTH <= (coordinates.first + font.FontWidth) ||
            OLED_HEIGHT <= (coordinates.second + font.FontHeight))
        {
            // Not enough space on current line
            return;
        }

        // Use the font to write
        for(i = 0; i < font.FontHeight; i++) {
            b = font.data[(c - 32) * font.FontHeight + i];
            for(j = 0; j < font.FontWidth; j++) {
                if((b << j) & 0x8000)  {
                    drawPixel({coordinates.first + j, (coordinates.second + i)}, color);
                } else {
                    drawPixel({coordinates.first + j, (coordinates.second + i)}, !color);
                }
            }
        }

        // The current space is now taken
        coordinates.first += font.FontWidth;

    }

    void writeSymbol(Icon icon, FontDef font, Color color) {
        writeSymbol(static_cast<char>(icon), font, color);
    }

    // Write full string to screenbuffer
    void writeString(string str, FontDef font, Color color) {
        //Write string to screenbuffer
        for(char c : str){
            writeSymbol(c, font, color);
        }
    }

    // Position the cursor
    void setCursor(pair<uint16_t, uint16_t> coord) {
        coordinates = coord;
    }

}