#include "oled.hpp"
#include "scheduler.hpp"

#include <libopencm3/stm32/timer.h>

#include <libopencm3/stm32/dma.h>
#include <libopencm3/stm32/dmamux.h>
#include <libopencm3/stm32/i2c.h>
#include <libopencm3/stm32/rcc.h>
#include <libopencm3/cm3/nvic.h>

using namespace std;

extern Scheduler oledSleep;

Oled::Oled oled;

Oled::Oled::Oled() : sleeping(false), initialized(false), coordinates({0,0}){ }

void Oled::Oled::wakeup(){
    this->sleeping = false;
}

void Oled::Oled::sleep(){
    this->sleeping = true;
}

bool Oled::Oled::isSleeping(){
    return this->sleeping;
}

void Oled::Oled::setCoordinates(pair<uint16_t, uint16_t> coord){
    this->coordinates = coord;
}

bool Oled::Oled::isInitialized(){
    return this->initialized;
}

void Oled::Oled::setInitialized(bool state){
    this->initialized = state;
}

bool Oled::Oled::isInverted(){
    return this->inverted;
}

void Oled::init(){

    //Setup the GPIO for I2C
    gpio_mode_setup(PORT_I2C1_SCL, GPIO_MODE_AF, GPIO_PUPD_PULLUP, GPIO_I2C1_SCL);
    gpio_mode_setup(PORT_I2C1_SDA, GPIO_MODE_AF, GPIO_PUPD_PULLUP, GPIO_I2C1_SDA);
    gpio_set_output_options(PORT_I2C1_SCL, GPIO_OTYPE_OD, GPIO_OSPEED_100MHZ, GPIO_I2C1_SCL);
    gpio_set_output_options(PORT_I2C1_SDA, GPIO_OTYPE_OD, GPIO_OSPEED_100MHZ, GPIO_I2C1_SDA);
	gpio_set_af(PORT_I2C1_SCL, GPIO_AF4, GPIO_I2C1_SCL);
	gpio_set_af(PORT_I2C1_SDA, GPIO_AF4, GPIO_I2C1_SDA);

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


    //Create data frame to configure the oled

    //Display Off
    oled.initBuffer.at(0) =  OLED_MEM_CMD;
    //Display Off
    oled.initBuffer.at(1) = 0xAE;
    //Memory addressing mode
    oled.initBuffer.at(2) = 0x20;
    //00,Horizontal Addressing Mode;
    //01,Vertical Addressing Mode;
    //10,Page Addressing Mode (RESET); 
    oled.initBuffer.at(3) = 0x10;
    //Set page start address
    oled.initBuffer.at(4) = 0xB0;
    //COM Out scan direction
    oled.initBuffer.at(5) = 0xC8;
    //Low column addressoled
    oled.initBuffer.at(6) = 0x00;
    //High column address
    oled.initBuffer.at(7) = 0x10;
    //Start line address
    oled.initBuffer.at(8) = 0x40;
    //Contrast
    oled.initBuffer.at(9) = 0x81;
    oled.initBuffer.at(10) = 0xFF;
    //Segment remap 0 to 127
    oled.initBuffer.at(11) = 0xA1;
    //Normal color
    oled.initBuffer.at(12) = 0xA6;
    //Set MUX ratio
    oled.initBuffer.at(13) = 0xA8;
    oled.initBuffer.at(14) = 0x3F;
    //Output follows RAM
    oled.initBuffer.at(15) = 0xA4;
    //Set oled offset
    oled.initBuffer.at(16) = 0xD3;
    oled.initBuffer.at(17) = 0x00;
    //Set oled clock divide ratio
    oled.initBuffer.at(18) = 0xD5;
    oled.initBuffer.at(19) = 0xF0;
    //Set precharge period
    oled.initBuffer.at(20) = 0xD9;
    oled.initBuffer.at(21) = 0x34;
    //Set COM pin HW config
    oled.initBuffer.at(22) = 0xDA;
    oled.initBuffer.at(23) = 0x12;
    //Set VCOMH (0.77*Vcc)
    oled.initBuffer.at(24) = 0xDB;
    oled.initBuffer.at(25) = 0x20;
    //Set DC-DC enable
    oled.initBuffer.at(26) = 0x8D;
    oled.initBuffer.at(27) = 0x14;
    //Turn on oled
    oled.initBuffer.at(28) = 0xAF;
    
    //Setup the DMA channel to get data from the prepared buffer
    dma_set_memory_address(DMA1, DMA_CHANNEL3, (uint32_t)&oled.initBuffer[0]);
    dma_set_number_of_data(DMA1, DMA_CHANNEL3, 29);
    i2c_set_bytes_to_transfer(I2C1, 29);
    //i2c_enable_interrupt(I2C1, I2C_CR1_TCIE);
    i2c_enable_autoend(I2C1);

    dma_enable_channel(DMA1, DMA_CHANNEL3);

    //Enable DMA and start sending data
    i2c_enable_txdma(I2C1);
    i2c_send_start(I2C1);
}

void Oled::Oled::update(){
    if(oled.isInitialized() && !oled.isSleeping()){
        //Prepare the buffer to contain frame data
        oled.pageBuffer.at(0) = OLED_MEM_CMD;
        oled.pageBuffer.at(1) = 0xB0;
        oled.pageBuffer.at(2) = 0x00;
        oled.pageBuffer.at(3) = 0x10;

        oled.screenBuffer.at(0)         = OLED_MEM_DAT;
        oled.screenBuffer.at(131)       = OLED_MEM_DAT;
        oled.screenBuffer.at(262)       = OLED_MEM_DAT;
        oled.screenBuffer.at(393)       = OLED_MEM_DAT;
        oled.screenBuffer.at(524)       = OLED_MEM_DAT;
        oled.screenBuffer.at(655)       = OLED_MEM_DAT;
        oled.screenBuffer.at(786)       = OLED_MEM_DAT;
        oled.screenBuffer.at(917)       = OLED_MEM_DAT;

        oled.dmaStatus = 0;
        oled.dmaIndex = 0;

        //Setup DMA to point to this buffer correctly
        dma_set_memory_address(DMA1, DMA_CHANNEL3, (uint32_t)&oled.pageBuffer[0]);
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
    dma_disable_channel(DMA1, DMA_CHANNEL3);
    dma_clear_interrupt_flags(DMA1, DMA_CHANNEL3, DMA_TCIF);
    nvic_clear_pending_irq(NVIC_DMA1_CHANNEL3_IRQ);

    if(oled.isInitialized()){
        if(oled.dmaStatus < 15){
            if(oled.dmaStatus % 2){
                oled.pageBuffer.at(1)++;
                oled.dmaIndex += 131;
                dma_set_memory_address(DMA1, DMA_CHANNEL3, (uint32_t)&oled.pageBuffer[0]);
                dma_set_number_of_data(DMA1, DMA_CHANNEL3, 4);
                i2c_set_bytes_to_transfer(I2C1, 4);
                dma_enable_channel(DMA1, DMA_CHANNEL3);
            }else{
                dma_set_memory_address(DMA1, DMA_CHANNEL3, (uint32_t)&oled.screenBuffer[oled.dmaIndex]);
                dma_set_number_of_data(DMA1, DMA_CHANNEL3, 131);
                i2c_set_bytes_to_transfer(I2C1, 131);
                dma_enable_channel(DMA1, DMA_CHANNEL3);
            }
            oled.dmaStatus++;
            i2c_enable_autoend(I2C1);
            i2c_send_start(I2C1);
        }else{
            dma_disable_channel(DMA1, DMA_CHANNEL3);
        }
    }else{
        dma_disable_channel(DMA1, DMA_CHANNEL3);
        i2c_disable_txdma(I2C1);
        oled.setInitialized(true);
    }

    dma_clear_interrupt_flags(DMA1, DMA_CHANNEL3, DMA_TCIF);
    nvic_clear_pending_irq(NVIC_DMA1_CHANNEL3_IRQ);
}


void oled_sleep_callback(void){
    oledSleep.pause();
    oled.fill(Oled::Color::BLACK);
    oled.update();
    oled.sleep();
}

void oled_wakeup_callback(void){
    oledSleep.reset();
    oledSleep.resume();
    oled.wakeup();
}

//Fill the whole screen with the given color
void Oled::Oled::fill(Color color) {
    for(uint32_t i = 0; i < screenBuffer.size(); i++) {
        if(i % 131){
            this->screenBuffer.at(i) = (color == Color::BLACK) ? 0x00 : 0xFF;
        }else continue;
    }
}


//Draw a pixel in the screenbuffer
void Oled::Oled::drawPixel(pair<uint16_t, uint16_t> coord, Color color) {

    coord.first += OLED_XOFFSET;
    coord.second += OLED_YOFFSET;
    
    if(coord.first >= OLED_WIDTH || coord.second >= OLED_HEIGHT) {
        // Don't write outside the buffer
        return;
    }

    // Check if pixel should be inverted
    if(this->isInverted()) {
       color = !color;
    }

    // Draw in the right color
    if(color == Color::WHITE) {
        this->screenBuffer.at(1 + (coord.second/8) + coord.first + (coord.second / 8) * OLED_WIDTH) |= 1 << (coord.second % 8);
    } else {
        this->screenBuffer.at(1 + (coord.second/8)+ coord.first + (coord.second / 8) * OLED_WIDTH) &= ~(1 << (coord.second % 8));
    }
}

//Draw a char to the screen buffer
void Oled::Oled::writeSymbol(char c, FontDef font, Color color) {
    uint32_t i, b, j;

    // Check remaining space on current line
    if (OLED_WIDTH <= (this->coordinates.first + font.FontWidth) ||
        OLED_HEIGHT <= (this->coordinates.second + font.FontHeight))
    {
        // Not enough space on current line
        return;
    }

    // Use the font to write
    for(i = 0; i < font.FontHeight; i++) {
        b = font.data[(c - 32) * font.FontHeight + i];
        for(j = 0; j < font.FontWidth; j++) {
            if((b << j) & 0x8000)  {
                this->drawPixel({this->coordinates.first + j, (this->coordinates.second + i)}, color);
            } else {
                this->drawPixel({this->coordinates.first + j, (this->coordinates.second + i)}, !color);
            }
        }
    }

    // The current space is now taken
    this->coordinates.first += font.FontWidth;

}

void Oled::Oled::writeSymbol(Icon icon, FontDef font, Color color) {
    this->writeSymbol(static_cast<char>(icon), font, color);
}

// Write full string to screenbuffer
void Oled::Oled::writeString(string str, FontDef font, Color color) {
    //Write string to screenbuffer
    for(char c : str){
        this->writeSymbol(c, font, color);
    }
}

// Position the cursor
void Oled::Oled::setCursor(pair<uint16_t, uint16_t> coord) {
    this->coordinates = coord;
}
