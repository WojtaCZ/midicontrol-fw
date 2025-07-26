#include "oled.hpp"
#include "scheduler.hpp"
#include "base.hpp"
#include "menu.hpp"

#include <stm32g431xx.h>
#include <core_cm4.h>
#include <cmsis_compiler.h>

//#include "stmcpp/gpio.hpp"
#include "stmcpp/register.hpp"

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
        //PA15 is SCL
        //PB7 is SDA

        // Set as AF
        stmcpp::reg::change(std::ref(GPIOA->MODER), GPIO_MODER_MODE15_Msk, 0b10, GPIO_MODER_MODE15_Pos);
        stmcpp::reg::change(std::ref(GPIOB->MODER), GPIO_MODER_MODE7_Msk, 0b10, GPIO_MODER_MODE7_Pos);

        // Set pins as open drain
        stmcpp::reg::change(std::ref(GPIOA->OTYPER), GPIO_OTYPER_OT15_Msk, 0b1, GPIO_OTYPER_OT15_Pos);
        stmcpp::reg::change(std::ref(GPIOB->OTYPER), GPIO_OTYPER_OT7_Msk, 0b1, GPIO_OTYPER_OT15_Pos);

        stmcpp::reg::change(std::ref(GPIOA->OSPEEDR), GPIO_OSPEEDR_OSPEED15_Msk, 0b11, GPIO_OSPEEDR_OSPEED15_Pos);
        stmcpp::reg::change(std::ref(GPIOB->OSPEEDR), GPIO_OSPEEDR_OSPEED7_Msk, 0b11, GPIO_OSPEEDR_OSPEED7_Pos);

        stmcpp::reg::change(std::ref(GPIOA->AFR[1]), GPIO_AFRH_AFSEL15_Msk, 4, GPIO_AFRH_AFSEL15_Pos);
        stmcpp::reg::change(std::ref(GPIOB->AFR[0]), GPIO_AFRL_AFSEL7_Msk, 4, GPIO_AFRL_AFSEL7_Pos);

    

        //Enable pullup on PA15
        GPIOA->PUPDR |= (0b01 << GPIO_PUPDR_PUPD15_Pos);
        //Enable pullup on PB7
        GPIOB->PUPDR |= (0b01 << GPIO_PUPDR_PUPD7_Pos);

        ///Set PA15 to be very high speed
        GPIOA->OSPEEDR |= (0b11 << GPIO_OSPEEDR_OSPEED15_Pos);
        ///Set PB7 to be very high speed
        GPIOB->OSPEEDR |= (0b11 << GPIO_OSPEEDR_OSPEED7_Pos);

        //Set PA15 to be AF4 (SCL)
        GPIOA->AFR[1] |= (4 << GPIO_AFRH_AFSEL15_Pos);
        //Set PB7 to be AF4 (SDA)
        GPIOB->AFR[0] |= (4 << GPIO_AFRL_AFSEL7_Pos);

        //Set up DMA priority to be high
        DMA1_Channel3->CCR |= (0b10 << DMA_CCR_PL_Pos);
        //Set direction to read from memory
        DMA1_Channel3->CCR |= (0b1 << DMA_CCR_DIR_Pos);
        //Enable memory increment mode
        DMA1_Channel3->CCR |= (0b1 << DMA_CCR_MINC_Pos);
        //Set DMA memory address
        DMA1_Channel3->CMAR = reinterpret_cast<uint32_t>(&initBuffer[0]);
        //Set DMA peripheral address
        DMA1_Channel3->CPAR = reinterpret_cast<uint32_t>(&I2C1->TXDR);
        //Set number of data to be the size of the initialization buffer
        DMA1_Channel3->CNDTR = initBuffer.size();
        //Enable transfer complete interrupt
        DMA1_Channel3->CCR |= (0b1 << DMA_CCR_TCIE_Pos);
        
        //Set up the DMA MUX request ID to be I2C1 TX
        DMAMUX1_Channel2->CCR |= (17 << DMAMUX_CxCR_DMAREQ_ID_Pos);
        
        I2C1->TIMINGR = 0x0070215B;
        //I2C1->TIMINGR = 0x00E057FD;
        //I2C1->TIMINGR = 0x20B0D9FF;

        //Set slave address (for 7bit address, the address needs to be shifted by 1)
        I2C1->CR2 |= (OLED_ADD << (I2C_CR2_SADD_Pos + 1));
        //Set up number of bytes to be transferred (the init frame size)
        I2C1->CR2 &= ~(I2C_CR2_NBYTES_Msk);
        I2C1->CR2 |= (initBuffer.size() << I2C_CR2_NBYTES_Pos);
        //Enable autoend mode
        I2C1->CR2 |= (0b1 << I2C_CR2_AUTOEND_Pos);

        //Enable the peripheral
        I2C1->CR1 |= (0b1 << I2C_CR1_PE_Pos);
        // Enable I2C1 clock stretching
        I2C1->CR1 &= ~(0b1 << I2C_CR1_NOSTRETCH_Pos);

        //Enable the DMA channel

        NVIC_EnableIRQ(DMA1_Channel3_IRQn);
        DMA1_Channel3->CCR |= (0b1 << DMA_CCR_EN_Pos);



        //Enable transmit DMA
        I2C1->CR1 |= (0b1 << I2C_CR1_TXDMAEN_Pos);
        //Generate start
        I2C1->CR2 |= (0b1 << I2C_CR2_START_Pos);
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



            //Set DMA memory address
            DMA1_Channel3->CMAR = reinterpret_cast<uint32_t>(&pageBuffer[0]);
            //Set number of data to be the size of the initialization buffer
            DMA1_Channel3->CNDTR = pageBuffer.size();
            //Set up number of bytes to be transferred (the init frame size)
            I2C1->CR2 &= ~(I2C_CR2_NBYTES_Msk);
            I2C1->CR2 |= (pageBuffer.size() << I2C_CR2_NBYTES_Pos);
            //Enable autoend mode
            I2C1->CR2 |= (0b1 << I2C_CR2_AUTOEND_Pos);

            //Enable the DMA channel
            DMA1_Channel3->CCR |= (0b1 << DMA_CCR_EN_Pos);
            //Enable transmit DMA
            I2C1->CR1 |= (0b1 << I2C_CR1_TXDMAEN_Pos);
            //Generate start
            I2C1->CR2 |= (0b1 << I2C_CR2_START_Pos);
        }
    }

    extern "C" void DMA1_Channel3_IRQHandler(void){
        //Disable the DMA channel
        DMA1_Channel3->CCR &= ~(0b1 << DMA_CCR_EN_Pos);
        //Clear the transfer complete flag
        DMA1->IFCR |= (0b1 << DMA_IFCR_CTCIF3_Pos);
        //Clear the pending IRQ
        NVIC_ClearPendingIRQ(DMA1_Channel3_IRQn);

        //Check if the init sequence has been done
        if(isInitialized()){
            if(dmaStatus < 15){
                if(dmaStatus % 2){
                    pageBuffer.at(1)++;
                    dmaIndex += 131;
                    DMA1_Channel3->CMAR = reinterpret_cast<uint32_t>(&pageBuffer[0]);
                    DMA1_Channel3->CNDTR = 4;
                    I2C1->CR2 &= ~(I2C_CR2_NBYTES_Msk);
                    I2C1->CR2 |= (4 << I2C_CR2_NBYTES_Pos);
                    DMA1_Channel3->CCR |= (0b1 << DMA_CCR_EN_Pos);
                }else{
                    DMA1_Channel3->CMAR = reinterpret_cast<uint32_t>(&screenBuffer[dmaIndex]);
                    DMA1_Channel3->CNDTR = 131;
                    I2C1->CR2 &= ~(I2C_CR2_NBYTES_Msk);
                    I2C1->CR2 |= (131 << I2C_CR2_NBYTES_Pos);
                    DMA1_Channel3->CCR |= (0b1 << DMA_CCR_EN_Pos);
                }
                dmaStatus++;
                I2C1->CR2 |= (0b1 << I2C_CR2_AUTOEND_Pos);
                I2C1->CR2 |= (0b1 << I2C_CR2_START_Pos);
            }else{
                DMA1_Channel3->CCR &= ~(0b1 << DMA_CCR_EN_Pos);
            }
        }else{
            DMA1_Channel3->CCR &= ~(0b1 << DMA_CCR_EN_Pos);
            I2C1->CR1 &= ~(0b1 << I2C_CR1_TXDMAEN_Pos);
            setInitialized(true);
        }

        DMA1->IFCR |= (0b1 << DMA_IFCR_CTCIF3_Pos);
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
