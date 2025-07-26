#include "oled.hpp"
#include "scheduler.hpp"
#include "base.hpp"
#include "menu.hpp"

#include <stm32g431xx.h>
#include <core_cm4.h>
#include <cmsis_compiler.h>

//#include "stmcpp/gpio.hpp"
#include "stmcpp/register.hpp"
#include "stmcpp/gpio.hpp"

//Scheduler used to time oled sleep
Scheduler oledSleepScheduler(OLED_SLEEP_INTERVAL, &Oled::sleepCallback, Scheduler::ACTIVE | Scheduler::DISPATCH_ON_INCREMENT);

array<uint8_t, OLED_SCREENBUF_SIZE> screenBuffer;

namespace Oled{
    //Coordinates on the oled
    pair<uint16_t, uint16_t> coordinates;
    //Flags
    bool inverted;
    bool initialized;
    bool sleeping;
    //Screen buffer storing image data
    
    //Buffers used for initialization
    array<uint8_t, 4> pageBuffer;

    //DMA flags
    uint8_t dmaStatus;
    uint16_t dmaIndex;

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

    void init(){

        // Set PA15 as SCL, PB7 as SDA
        stmcpp::gpio::pin<stmcpp::gpio::port::porta, 15> oledScl (stmcpp::gpio::mode::af4, stmcpp::gpio::otype::openDrain, stmcpp::gpio::speed::veryHigh, stmcpp::gpio::pull::pullUp);
        stmcpp::gpio::pin<stmcpp::gpio::port::portb, 7> oledSda (stmcpp::gpio::mode::af4, stmcpp::gpio::otype::openDrain, stmcpp::gpio::speed::veryHigh, stmcpp::gpio::pull::pullUp);


        stmcpp::reg::write(std::ref(DMA1_Channel3->CCR), 
            (0b10 << DMA_CCR_PL_Pos) | // High priority
            (0b1 << DMA_CCR_DIR_Pos) | // Memory to peripheral direction
            (0b1 << DMA_CCR_MINC_Pos) | // Memory increment mode
            (0b1 << DMA_CCR_TCIE_Pos)   // Transfer complete interrupt enable
            
        );

        // Set up the DMA memory and peripheral
        stmcpp::reg::write(std::ref(DMA1_Channel3->CMAR), reinterpret_cast<uint32_t>(&initBuffer[0]));
        stmcpp::reg::write(std::ref(DMA1_Channel3->CPAR), reinterpret_cast<uint32_t>(&I2C1->TXDR));
        stmcpp::reg::write(std::ref(DMA1_Channel3->CNDTR), initBuffer.size());

        // Set up DMAMUX routing (DMAMUX channels start from 0, DMA from 1, thats why the offset in numbering)
        stmcpp::reg::write(std::ref(DMAMUX1_Channel2->CCR), (17 << DMAMUX_CxCR_DMAREQ_ID_Pos));

        // Set up I2C
        stmcpp::reg::write(std::ref(I2C1->CR1), I2C_CR1_TXDMAEN); 
        stmcpp::reg::write(std::ref(I2C1->CR2), 
            (0b1 << I2C_CR2_AUTOEND_Pos) | // Autoend mode
            (OLED_ADD << (I2C_CR2_SADD_Pos + 1)) | // Device address
            (initBuffer.size() << I2C_CR2_NBYTES_Pos) // Buffer size
        );

        // Set up I2C timing to be 1MHz (super speed)
        stmcpp::reg::write(std::ref(I2C1->TIMINGR), 0x0070215B);
        
        // Enable DMA and TX complete interrupt
        stmcpp::reg::set(std::ref(DMA1_Channel3->CCR), DMA_CCR_EN);
        NVIC_EnableIRQ(DMA1_Channel3_IRQn);

        //Enable the I2C peripheral 
        stmcpp::reg::set(std::ref(I2C1->CR1), I2C_CR1_PE);

        //Generate start
        stmcpp::reg::set(std::ref(I2C1->CR2), I2C_CR2_START);
    }

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


            // Reinitialize the DMA and SPI buffer to use the image buffer instead
            stmcpp::reg::write(std::ref(DMA1_Channel3->CMAR), reinterpret_cast<uint32_t>(&pageBuffer[0]));
            stmcpp::reg::write(std::ref(DMA1_Channel3->CNDTR), pageBuffer.size());
            stmcpp::reg::change(std::ref(I2C1->CR2), 0xff, pageBuffer.size(), I2C_CR2_NBYTES_Pos);

            // Reenable DMA
            stmcpp::reg::set(std::ref(DMA1_Channel3->CCR), DMA_CCR_EN);

            //Generate start
            stmcpp::reg::set(std::ref(I2C1->CR2), I2C_CR2_START);
        }
    }

    extern "C" void DMA1_Channel3_IRQHandler(void){
        //Disable DMA and clear the flag
        stmcpp::reg::clear(std::ref(DMA1_Channel3->CCR), DMA_CCR_EN);
    

        //Check if the init sequence has been done
        if(isInitialized()){
            if(dmaStatus < 15){
                if(dmaStatus % 2){
                    pageBuffer.at(1)++;
                    dmaIndex += 131;

                    // Reinitialize the DMA and SPI buffer to use the image buffer instead
                    stmcpp::reg::write(std::ref(DMA1_Channel3->CMAR), reinterpret_cast<uint32_t>(&pageBuffer[0]));
                    stmcpp::reg::write(std::ref(DMA1_Channel3->CNDTR), 4);
                    stmcpp::reg::change(std::ref(I2C1->CR2), 0xff, 4, I2C_CR2_NBYTES_Pos);

                    
                }else{

                    // Reinitialize the DMA and SPI buffer to use the image buffer instead
                    stmcpp::reg::write(std::ref(DMA1_Channel3->CMAR), reinterpret_cast<uint32_t>(&screenBuffer[dmaIndex]));
                    stmcpp::reg::write(std::ref(DMA1_Channel3->CNDTR), 131);
                    stmcpp::reg::change(std::ref(I2C1->CR2), 0xff, 131, I2C_CR2_NBYTES_Pos);
                }

                // Reenable DMA
                stmcpp::reg::set(std::ref(DMA1_Channel3->CCR), DMA_CCR_EN);

                dmaStatus++;

                //Generate start
                stmcpp::reg::set(std::ref(I2C1->CR2), I2C_CR2_START);

            }else{
                //Disable DMA
                stmcpp::reg::clear(std::ref(DMA1_Channel3->CCR), DMA_CCR_EN);
            }
        }else{
            //Disable DMA
            stmcpp::reg::clear(std::ref(DMA1_Channel3->CCR), DMA_CCR_EN);
            setInitialized(true);
        }

        //Clear the pending IRQ
        stmcpp::reg::set(std::ref(DMA1->IFCR), DMA_IFCR_CTCIF3);
        NVIC_ClearPendingIRQ(DMA1_Channel3_IRQn);
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
