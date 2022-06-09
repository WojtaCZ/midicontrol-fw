#include "comm.hpp"
#include "usb.h"
#include "scheduler.hpp"
#include "base.hpp"
#include "menu.hpp"
#include "display.hpp"
#include "ble.hpp"

#include <string.h>
#include <stdlib.h>
/*
MenuItem itms[10];
extern MenuItem menuitem_back;
extern Menu menu_main;*/
//Menu menu_songlist = {{"Seznam pisni","Song list"}, .items = {0,0,0,0,0,0,0,0,0,0}, .parent = &menu_main};


extern Scheduler sched_comm_decode;
extern Scheduler sched_comm_timeout;

uint8_t commUsbFifo[COMM_USB_FIFO_SIZE];
uint8_t commUsbGotFlag;
uint16_t commUsbMsgLen, commUsbFifoIndex;

uint8_t commUsartFifo[COMM_USART_FIFO_SIZE];


int comm_awaiting_response = 0;

struct cmd_handler commands[] = {
{"music songlist", &comm_music_songlist}, 
{"music play", &comm_music_play}, 
{"music record", &comm_music_record}, 
{"music stop", &comm_music_stop},
{"display status", &comm_display_status},
{"display song", &comm_display_song},
{"display verse", &comm_display_verse},
{"display letter", &comm_display_letter},
{"display led", &comm_display_led},
{"base usb status", &comm_base_usb_status},
{"base isource status", &comm_base_isource_status},
{"base bluetooth status", &comm_base_bluetooth_status},
{"base bluetooth send", &comm_base_bluetooth_send},
{"base midi status", &comm_base_midi_status},
{"base midi config", &comm_base_midi_config}
};


void comm_base_bluetooth_send(char * param){
	//ble_send(&param[4], strlen(&param[4]));
}


//Handler for music songlist command
/*void comm_music_songlist(char * param){

	if(strstr(param, "get")){
		
	}else if(strstr(param, "set")){

	}else if(strstr(param, "resp")){

		int idx = 0;
		
		char * tok = strtok(param+6, "\n");
		while(tok != NULL){
			//itms = realloc(*itms, idx*sizeof(MenuItem));
			itms[idx].text[0] = (char *) calloc((unsigned)strlen(tok)+1, sizeof(unsigned char));
			memcpy(itms[idx].text[0], tok, strlen(tok)+1);
			itms[idx].text[1] = (char *) calloc((unsigned)strlen(tok)+1, sizeof(unsigned char));
			memcpy(itms[idx].text[1], tok, strlen(tok)+1);
			itms[idx].flags = MENU_CALLBACK_IS_FUNCTION;
			itms[idx].callback = &base_play;

			tok = strtok(NULL, "\n");
			idx++;
		}


		for(int i = 0; i < idx; i++){
			//menu_songlist.items[i]->text[0] = (char *) calloc((unsigned)strlen(itms[i].text[0]), sizeof(unsigned char));
			//memcpy(menu_songlist.items[i], &itms[i], sizeof(itms[i]));
			//menu_songlist.items[i] = &itms[i];
		}

		//menu_songlist.items[idx] = &menuitem_back;
	
		menu_show(&menu_songlist);

	}
	
}*/

void comm_music_play(char * param){
	if(strstr(param, "get")){
		//Signal not supported param combination
		comm_send_ns();
	}else if(strstr(param, "set")){
		
	}else if(strstr(param, "resp")){

	}
}

void comm_music_record(char * param){
	if(strstr(param, "get")){
		//Signal not supported param combination
		comm_send_ns();
	}else if(strstr(param, "set")){

	}else if(strstr(param, "resp")){

	}
}

void comm_music_stop(char * param){
	if(strstr(param, "get")){
		//Signal not supported param combination
		comm_send_ns();
	}else if(strstr(param, "set")){

	}else if(strstr(param, "resp")){

	}
}

void comm_display_status(char * param){
	if(strstr(param, "get")){
		
	}else if(strstr(param, "set")){

	}else if(strstr(param, "resp")){

	}
}

void comm_display_song(char * param){
	if(strstr(param, "get")){
		
	}else if(strstr(param, "set")){

	}else if(strstr(param, "resp")){

	}
}

void comm_display_verse(char * param){
	if(strstr(param, "get")){
		
	}else if(strstr(param, "set")){

	}else if(strstr(param, "resp")){

	}
}

void comm_display_letter(char * param){
	if(strstr(param, "get")){
		
	}else if(strstr(param, "set")){

	}else if(strstr(param, "resp")){

	}
}
void comm_display_led(char * param){
	if(strstr(param, "get")){
		switch(display_get_led()){
			case DISPLAY_LED_R:
				comm_send_text("R\n");
			break;

			case DISPLAY_LED_G:
				comm_send_text("G\n");
			break;

			case DISPLAY_LED_B:
				comm_send_text("B\n");
			break;

			case DISPLAY_LED_Y:
				comm_send_text("Y\n");
			break;

			case DISPLAY_LED_OFF:
				comm_send_text("O\n");
			break;

			default:
				comm_send_error();
		}
	}else if(strstr(param, "set")){
		display_set_led(param[4], param[6]-48);
	}else if(strstr(param, "resp")){
		comm_send_ns();
	}
}
void comm_base_usb_status(char * param){
	if(strstr(param, "get")){
		comm_send_ns();
	}else if(strstr(param, "set")){
		comm_send_ns();
	}else if(strstr(param, "resp")){
		comm_send_ns();
	}
}

void comm_base_isource_status(char * param){
	/*if(strstr(param, "get")){
		char buff[3];
		buff[0] = base_get_current_source() + 48;
		buff[1] = '\n';
		usb_cdc_tx(buff, 3);
	}else if(strstr(param, "set")){

		if(strstr(param+4, "1")){
			base_set_current_source(1);
		}else if(strstr(param+4, "0")){
			base_set_current_source(0);
		}
	}else if(strstr(param, "resp")){
		char bf[128];
		sprintf(bf, "Got response for command %s %s\n", (param[0]&0x80) ? "SET" : "GET", commands[(param[0]&0x7f) - 1]);
		usb_cdc_tx(bf, strlen(bf));
	}*/
}

void comm_base_bluetooth_status(char * param){}
void comm_base_midi_status(char * param){}
void comm_base_midi_config(char * param){}



void comm_send_text(char * msg){
	usb_cdc_tx(msg, strlen(msg));
}

void comm_send_error(){
	usb_cdc_tx((void*)"error\n", 6);
}

void comm_send_ns(){
	usb_cdc_tx((void*)"not supported\n", 14);
}



void comm_cmd_send(uint8_t cmd_type, char * payload){
/*	comm_awaiting_response = cmd_type;
	char buff[254];
	memset(buff, 0, 254);
	sprintf(buff, "%s %s %s\n", (cmd_type&0x80) ? "set" : "get", commands[(cmd_type&0x7f)-1].cmd, payload);
	usb_cdc_tx(buff, strlen(buff));
	sched_comm_timeout.flags = 0 | SCHEDULER_ON;*/
}


void comm_timeout_callback(){
	/*comm_awaiting_response = 0;
	sched_comm_timeout.flags = 0;
	usb_cdc_tx((void*)"TO\n", 4);*/
}


void comm_usb_packet_received(uint8_t * buff, int len){
	char param[128];
	
	if(comm_awaiting_response){
		param[0] = comm_awaiting_response;
		sprintf(param+1, "resp %s", buff);
		(commands[(comm_awaiting_response&0x7f)-1].recv_callback)(param);
		/*sched_comm_timeout.flags = 0;
		sched_comm_timeout.counter = 0;*/
		
	}else{
		int found = 0;

		for(int i = 0; i < CMD_COUNT; i++){
			char * idx;
			if((idx = strstr((char*)buff, commands[i].cmd)) > 0){
				
				if(strstr((char*)buff, "get") > 0){
					sprintf(param, "get");
					found = 1;
					(commands[i].recv_callback)(param);
				}else if(strstr((char*)buff, "set") > 0){
					found = 1;
					sprintf(param, "set %s", (idx + strlen(commands[i].cmd)));
					(commands[i].recv_callback)(param);
				}else{
					usb_cdc_tx((void*)"invalid\n", 8);
				}

			}
		}

		if(!found) usb_cdc_tx((void*)"invalid\n", 8);
	}
	


}

