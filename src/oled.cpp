#include "oled.hpp"
#include "wdg.hpp"
#include <stmcpp/scheduler.hpp>
#include "base.hpp"

#include <stm32g431xx.h>
#include <core_cm4.h>
#include <cmsis_compiler.h>
#include <cstring>

#include "stmcpp/register.hpp"
#include "stmcpp/gpio.hpp"
#include "stmcpp/systick.hpp"

//Scheduler used to time oled sleep
stmcpp::scheduler::Scheduler oledSleepScheduler(OLED_SLEEP_INTERVAL, &Oled::sleepCallback, false, true);

uint8_t screenBuffer[OLED_SCREENBUF_SIZE];

Oled::OledBuffer frameBuffer;

namespace Oled{
    //Coordinates on the oled
    pair<uint16_t, uint16_t> coordinates;
    //Flags
    bool inverted;
    volatile bool initialized;
    bool sleeping;

    // Snapshot of the frame being sent: the GUI keeps drawing into frameBuffer, DMA reads from here, so a half-redrawn
    // page never reaches the display. It also matches the content of the display RAM, only
    // changed pages are sent.
    uint8_t txBuffer[OledBuffer::BUFFER_SIZE];
    // Display RAM content unknown (after initialization): send all pages
    bool fullFrame = true;

    // Page address (page start, column 0), one 4-byte block per page
    const uint8_t pageCmd[8][4] = {
        {OLED_MEM_CMD, 0xb0, 0x00, 0x10}, {OLED_MEM_CMD, 0xb1, 0x00, 0x10},
        {OLED_MEM_CMD, 0xb2, 0x00, 0x10}, {OLED_MEM_CMD, 0xb3, 0x00, 0x10},
        {OLED_MEM_CMD, 0xb4, 0x00, 0x10}, {OLED_MEM_CMD, 0xb5, 0x00, 0x10},
        {OLED_MEM_CMD, 0xb6, 0x00, 0x10}, {OLED_MEM_CMD, 0xb7, 0x00, 0x10},
    };

    // List of the frame's blocks; the next block is started by the DMA interrupt after the previous one completes
    constexpr int MAX_CHUNKS = 17; // 8 × (address + page) + display on
    const uint8_t * chunkPtr[MAX_CHUNKS];
    uint16_t chunkLen[MAX_CHUNKS];
    uint8_t chunkCount;
    volatile uint8_t chunkIndex;
    // A frame on the bus; a new one is started only after completion (process()), never in the middle of a transfer
    volatile bool busy;
    bool framePending;

    // Display on, appended after the first frame following initialization or wakeup
    const uint8_t displayOnCmd[] = {OLED_MEM_CMD, 0xaf};
    bool displayOn = false;
    // Display off (sleep); sent alone as soon as the bus is free
    const uint8_t sleepCmd[] = {OLED_MEM_CMD, 0xae};
    bool sleepPending;

    const uint8_t initBuffer[] = {
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
        0x14
        // The display stays off: the OLED RAM survives an MCU reset (last screen), it is turned on
        // only at the end of the first frame
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
        stmcpp::reg::write(std::ref(DMA1_Channel3->CNDTR), sizeof(initBuffer));

        // Set up DMAMUX routing (DMAMUX channels start from 0, DMA from 1, thats why the offset in numbering)
        stmcpp::reg::write(std::ref(DMAMUX1_Channel2->CCR), (17 << DMAMUX_CxCR_DMAREQ_ID_Pos));

        // Set up I2C
        stmcpp::reg::write(std::ref(I2C1->CR1), I2C_CR1_TXDMAEN); 
        stmcpp::reg::write(std::ref(I2C1->CR2), 
            (0b1 << I2C_CR2_AUTOEND_Pos) | // Autoend mode
            (OLED_ADD << (I2C_CR2_SADD_Pos + 1)) | // Device address
            (sizeof(initBuffer) << I2C_CR2_NBYTES_Pos) // Buffer size
        );

        // I2C1 kernel clock = PCLK1 = 144 MHz (RCC_CCIPR.I2C1SEL = 00), target 400 kHz (Fast mode, SH1106 max.)
        // PRESC = 3 → t = 4 / 144 MHz = 27.8 ns
        // SCLL = 0x35 → tLOW  = 54 × 27.8 ns = 1.50 us (min. 1.3 us)
        // SCLH = 0x1D → tHIGH = 30 × 27.8 ns = 0.83 us (min. 0.6 us)
        // SDADEL = 2 (56 ns), SCLDEL = 0xF (444 ns ≥ tr 300 ns + tSU;DAT 100 ns)
        // Period 2.33 us + synchronization/rise time ≈ 2.5 us → ~400 kHz
        // (the original 0x0070215B was a value for 16 MHz → at 144 MHz ~1 MHz SCL)
        stmcpp::reg::write(std::ref(I2C1->TIMINGR), 0x30F21D35);
        
        // Enable DMA and TX complete interrupt
        stmcpp::reg::set(std::ref(DMA1_Channel3->CCR), DMA_CCR_EN);
        NVIC_EnableIRQ(DMA1_Channel3_IRQn);

        //Enable the I2C peripheral 
        stmcpp::reg::set(std::ref(I2C1->CR1), I2C_CR1_PE);

        //Generate start
        stmcpp::reg::set(std::ref(I2C1->CR2), I2C_CR2_START);
    }

    //Wakeup function for the oled: the current frame is sent first, the display is turned on after it
    void wakeup(){
        if(isSleeping()){
            sleeping = false;
            sleepPending = false;
            displayOn = false;
            framePending = true;
        }
    }

    //Sleep function for the oled (display off; the frame buffer is kept, frames are not sent while sleeping)
    void sleep(){
        if(!isSleeping()){
            sleeping = true;
            sleepPending = true;
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

    void startChunk(uint8_t idx){
        stmcpp::reg::write(std::ref(DMA1_Channel3->CMAR), reinterpret_cast<uint32_t>(chunkPtr[idx]));
        stmcpp::reg::write(std::ref(DMA1_Channel3->CNDTR), chunkLen[idx]);
        stmcpp::reg::change(std::ref(I2C1->CR2), 0xff, chunkLen[idx], I2C_CR2_NBYTES_Pos);

        // Reenable DMA
        stmcpp::reg::set(std::ref(DMA1_Channel3->CCR), DMA_CCR_EN);

        //Generate start
        stmcpp::reg::set(std::ref(I2C1->CR2), I2C_CR2_START);
    }

    // Starts the pending frame as soon as the bus is free (called from the main loop)
    void process(){
        if(!isInitialized() || busy) return;

        if(sleepPending){
            sleepPending = false;
            chunkPtr[0] = sleepCmd;
            chunkLen[0] = sizeof(sleepCmd);
            chunkCount = 1;
            chunkIndex = 0;
            busy = true;
            startChunk(0);
            return;
        }

        if(isSleeping() || !framePending) return;
        framePending = false;

        constexpr int STRIDE = OledBuffer::STRIDE;
        uint8_t count = 0;
        for(int p = 0; p < OledBuffer::PAGES; p++){
            const uint8_t * src = frameBuffer.getBufferAddress(p * STRIDE);
            uint8_t * dst = &txBuffer[p * STRIDE];
            if(!fullFrame && std::memcmp(src, dst, STRIDE) == 0) continue;
            std::memcpy(dst, src, STRIDE);
            chunkPtr[count]   = pageCmd[p];
            chunkLen[count++] = sizeof(pageCmd[p]);
            chunkPtr[count]   = dst;
            chunkLen[count++] = STRIDE;
        }
        fullFrame = false;
        if(!displayOn){
            chunkPtr[count]   = displayOnCmd;
            chunkLen[count++] = sizeof(displayOnCmd);
            displayOn = true;
        }
        if(count == 0) return;

        chunkCount = count;
        chunkIndex = 0;
        busy = true;
        startChunk(0);
    }

    void update(){
        framePending = true;
        process();
    }

    // noinline + used: test_fault.py calls it via gdb (a full flush under the watchdog)
    [[gnu::used, gnu::noinline]] void flush(uint32_t timeoutMs){
        uint32_t start = stmcpp::systick::getTicks();
        while((!isInitialized() || busy || sleepPending || (framePending && !isSleeping())) && (stmcpp::systick::getTicks() - start) < timeoutMs){
            wdg::kick(); // waits up to timeoutMs (at startup: 500 ms)
            process();
        }
    }

    extern "C" void DMA1_Channel3_IRQHandler(void){
        //Disable DMA and clear the flag
        stmcpp::reg::clear(std::ref(DMA1_Channel3->CCR), DMA_CCR_EN);


        //Check if the init sequence has been done
        if(isInitialized()){
            uint8_t next = chunkIndex + 1;
            if(busy && next < chunkCount){
                chunkIndex = next;
                startChunk(next);
            }else{
                busy = false;
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
        sleep();
    }

    //Callback to wakeup the oled, (re)starts the inactivity timer
    void wakeupCallback(void){
        oledSleepScheduler.start();
        wakeup();
    }

    // Constructor
    OledBuffer::OledBuffer() {
        // 1. Clear all memory to 0 (Black)
        std::memset(buffer, 0, BUFFER_SIZE);

        // 2. Initialize the Control Bytes at the start of every page
        // Layout: [CMD, D0, D1... D129], [CMD, D0... ]
        for (int p = 0; p < PAGES; p++) {
            buffer[p * STRIDE] = OLED_MEM_DAT;
        }
    }


    void OledBuffer::setPixel(int x, int y, uint8_t color) {
        if (x < 0 || x >= WIDTH || y < 0 || y >= HEIGHT) return;

        // Convert (x,y) to Page/Bit format
        int page = y / 8;       // Which horizontal band (0-7)
        int bit  = y % 8;       // Which bit inside the byte (0-7)

        // Calculate index: (Page Start) + (Control Offset) + X
        int index = (page * STRIDE) + CONTROL_BYTES + x;

        if (color > 0) {
            buffer[index] |= (1 << bit); // Set bit
        } else {
            buffer[index] &= ~(1 << bit); // Clear bit
        }
    }

    uint8_t OledBuffer::getPixel(int x, int y) const {
        if (x < 0 || x >= WIDTH || y < 0 || y >= HEIGHT) return 0;

        int page = y / 8;
        int bit  = y % 8;
        int index = (page * STRIDE) + CONTROL_BYTES + x;

        return (buffer[index] & (1 << bit)) ? 1 : 0;
    }

    int OledBuffer::getWidth() const {
        return WIDTH;
    }

    int OledBuffer::getHeight() const {
        return HEIGHT;
    }

    uint8_t * OledBuffer::getBufferAddress(int offset) {
        return &buffer[offset];
    }

    int OledBuffer::getBufferSize() const {
        return BUFFER_SIZE;
    }
}
