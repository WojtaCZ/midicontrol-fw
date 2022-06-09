#include "display.hpp"

#include <string.h>
#include <stdlib.h>

/* PROTOCOL NOTE
Display expects 9byte frames over inverted USART. In each byte, 0xe in MSW (most significant word) means the segment is turned off. Frame structure as follows 
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

//Default state - all off
uint8_t state[] = {0xb0, 0xe0, 0xe0, 0xe0, 0xe0, 0xe0, 0xe0, 0xe0, 0xe0};
//Signalize state change in any of the state bytes - results in transmission of the whole buffer
uint8_t state_change = 1;

void display_set_song(uint16_t song, uint8_t visible){
    if(visible){
        if(song > 1999) song = 1999;
        state[3] = (song - ((uint16_t)(song/10)*10)) & 0x0f;
        state[4] = (song - ((uint16_t)(song/100)*100))/10 & 0x0f;
        state[5] = (song - ((uint16_t)(song/1000)*1000))/100 & 0x0f;
        state[6] = (song/1000) & 0x0f;
    }else{
        state[3] = 0xe0;
        state[4] = 0xe0;
        state[5] = 0xe0;
        state[6] = 0xe0;
    }
}

void display_set_verse(uint8_t verse, uint8_t visible){
    if(visible){
        if(verse > 99) verse = 99;
        state[1] = (verse - ((uint8_t)(verse/10)*10)) & 0x0f;
        state[2] = (verse/10) & 0x0f;
    }else{
        state[1] = 0xe0;
        state[2] = 0xe0;
    }
}

void display_set_letter(char letter, uint8_t visible){
    if(visible){
        if(letter > 'E' || letter < 'A') letter = 'A';
        state[7] = 0x0f & (letter - 55);
    }else{
        state[7] = 0xe0;
    }
}

void display_set_led(uint8_t led, uint8_t visible){
    if(visible){
        state[8] = 0x07 & led;
    }else{
        state[8] = 0xe0;
    }
}


uint16_t display_get_song(){
    return (state[3] & 0x0f) + (state[4] & 0x0f)*10 + (state[5] & 0x0f)*100 + (state[6] & 0x0f)*1000;
}

uint8_t display_get_verse(){
    return (state[1] & 0x0f) + (state[2] & 0x0f)*10;
}

char display_get_letter(){
    return (state[7] & 0x0f) + 55;
}

uint8_t display_get_led(){
    return (state[8] & 0x07);
}



