#include "menu.hpp"
#include "oled.hpp"
#include "usb.h"
#include "scheduler.hpp"


#include <string>
#include <algorithm>


using namespace std;

Scheduler guiRenderScheduler(30, &GUI::render, Scheduler::PERIODICAL | Scheduler::ACTIVE | Scheduler::DISPATCH_ON_INCREMENT);
Scheduler menuScrollScheduler(2000, &GUI::scroll_callback, Scheduler::PERIODICAL | Scheduler::DISPATCH_ON_INCREMENT);

namespace GUI{

	bool forceRender = 0;
	int scrollIndex = 0;
	bool hadScrollPause = false;
	Language LANG = Language::CS;

	Item itm_play({{Language::EN, "Play"},{Language::CS, "Prehraj mi tu songu brasko"}});
	Item itm_record({{Language::EN, "Record"},{Language::CS, "Nahraj mi to audio"}});
	Menu menu_main({{Language::EN, "Main menu"},{Language::CS, "Hlavni menu"}}, {itm_play, itm_record});

	Menu & activeMenu = menu_main;

	//Menu constructor
	Menu::Menu(const map<Language, string> title, vector<Item> itms) : title(title), items(itms){ }

	//Menu item constructor
	Item::Item(const map<Language, string> title, CallbackType clbType) : title(title), callbackType(CallbackType::NONE){

	}
	/*
	Menu::Checkbox::Checkbox(const map<Language, string>  & title, CallbackType clbType = CallbackType::NONE, bool checked = false){
		this->title = title;
		this->callbackType = clbType;
		this->icon.SELECTED = Oled::Icon::CHECKBOX_SEL_UNCH;
		this->icon.NOT_SELECTED = Oled::Icon::CHECKBOX_UNSEL_UNCH;
	}*/

	void display(Menu & menu){
		activeMenu = menu;
	}

	int Menu::getSelectedIndex(){
		return this->selectedIndex;
	}

	int Menu::setSelectedIndex(int index){
		if(index > static_cast<int>(this->items.size()) || index < 0) return 0;
		this->selectedIndex = index;
		return 1;
	}

	bool Menu::selectedIndexChanged(){
		if(this->selectedIndex != this->lastSelectedIndex) return true;
		return false;
	}

	bool Menu::hasTitle(Language lang){
		//Check if the menu has title in the specified language
		if(this->title.find(lang) != this->title.end()) return true;
		return false;
	}

	bool Menu::selectedFirstItem(){
		//If the first menu item is selected
		if(this->selectedIndex == 0) return true;
		return false;
	}

	bool Menu::selectedLastItem(){
		//If the last menu item is selected
		if(this->selectedIndex == static_cast<int>(this->items.size())) return true;
		return false;
	}


	void render(void){
		//If there is nothing to update, return
		if(!activeMenu.selectedIndexChanged() && !forceRender) return;
		//Zero out the force render flag
		forceRender = 0;
		//Blank the oled
		Oled::fill(Oled::Color::BLACK);

		//If the menu has a title in selected language, display it
		if(activeMenu.hasTitle(LANG)){
			Oled::setCursor({0,0});
			Oled::writeString(activeMenu.title.at(LANG), Font_7x10, Oled::Color::WHITE);
		}

		//Display arrows indicating more items on top or bottom of the menu
		if(activeMenu.selectedFirstItem()){
			//Down arrow
			Oled::setCursor({117,41});
			Oled::writeSymbol(Oled::Icon::DOWN_ARROW, Icon_11x18, Oled::Color::WHITE);
		}else if(activeMenu.selectedLastItem()){
			//Up arrow
			Oled::setCursor({117,19});
			Oled::writeSymbol(Oled::Icon::UP_ARROW, Icon_11x18, Oled::Color::WHITE);
		}else{
			//Down arrow
			Oled::setCursor({117,41});
			Oled::writeSymbol(Oled::Icon::DOWN_ARROW, Icon_11x18, Oled::Color::WHITE);
			//Up arrow
			Oled::setCursor({117,19});
			Oled::writeSymbol(Oled::Icon::UP_ARROW, Icon_11x18, Oled::Color::WHITE);
		}

		//If the text is too long to display, enable scrolling
		if(activeMenu.items.at(activeMenu.getSelectedIndex()).title.at(LANG).length() > 9 && !menuScrollScheduler.isActive()){
			scrollIndex = 0;
			hadScrollPause = false;
			menuScrollScheduler.pause();
			menuScrollScheduler.setInterval(MENU_SCROLL_PAUSE);
			menuScrollScheduler.reset();
			menuScrollScheduler.resume();
		}else if (activeMenu.items.at(activeMenu.getSelectedIndex()).title.at(LANG).length() < 9) menuScrollScheduler.pause();

		int indexShift = 1;

		if(activeMenu.selectedFirstItem()){
			indexShift = 0;
		}else if(activeMenu.selectedLastItem()){
			indexShift = 2;
		}
		

		//int i = this->activeMenu.getSelectedIndex()-indexShift; i < this->activeMenu.getSelectedIndex()-indexShift+2; i++

		for(int i = 0; i < 2; i++){
			//Draw the icon next to menu item
			Oled::setCursor({0 + MENU_LEFT_OFFSET, ROW(i) + MENU_TOP_OFFSET + MENU_TEXT_SPACING*i});
			Oled::writeSymbol(i == indexShift ? (activeMenu.items.at(i).iconSelected) : (activeMenu.items.at(i).iconNotSelected) , MENU_ICON_FONT, Oled::Color::WHITE);
			//Draw the menu item
			Oled::setCursor({COL(1) + MENU_LEFT_OFFSET + MENU_SELECTOR_SPACING, ROW(i) + MENU_TOP_OFFSET + MENU_TEXT_SPACING*i});
			//Title reduced to 9 characters to fit on screen
			string reducedTitle = activeMenu.items.at(i).title.at(LANG).substr(i == indexShift ? scrollIndex : 0, 8);
			Oled::writeString(reducedTitle, MENU_FONT, Oled::Color::WHITE);
			
		}

		Oled::update();
	}

	void scroll_callback(){
		if(hadScrollPause){
			if(scrollIndex < activeMenu.items.at(activeMenu.getSelectedIndex()).title.at(LANG).length() - 9){
				scrollIndex++;
			}else if(scrollIndex == activeMenu.items.at(activeMenu.getSelectedIndex()).title.at(LANG).length() - 9){
				menuScrollScheduler.pause();
				menuScrollScheduler.setInterval(MENU_SCROLL_PAUSE);
				menuScrollScheduler.reset();
				menuScrollScheduler.resume();
				scrollIndex++;
			}else{
				hadScrollPause = false;
				menuScrollScheduler.pause();
				menuScrollScheduler.setInterval(MENU_SCROLL_PAUSE);
				menuScrollScheduler.reset();
				scrollIndex = 0;
				menuScrollScheduler.resume();
			}

		    forceRender = true;
		}else{
			menuScrollScheduler.setInterval(500);
			hadScrollPause = true;
		}
	}

}

/*

void menu_update(void){

		i = 0;
		while((i + menuTopPos) < len && i < MENU_LINES){
			int index = menuTopPos + i;
			if(menuItem == index && MENU_LINES > 1){
				oled_set_cursor(0 + MENU_LEFT_OFFSET, ROW(i) + MENU_TOP_OFFSET + MENU_TEXT_SPACING*i);
				if(menu->items[index]->specSelected != 0 && !(menu->items[index]->flags & MENU_ITEM_IS_CHECKBOX)){
					oled_write_char(menu->items[index]->specSelected, MENU_ICON_FONT, White);
				}else if(menu->items[index]->flags & MENU_ITEM_IS_CHECKBOX){
					if(menu->items[index]->flags & MENU_ITEM_IS_CHECKED){
						oled_write_char(MENU_SELECTED_CHECKBOX_CHECKED_SYMBOL, MENU_ICON_FONT, White);
					}else{
						oled_write_char(MENU_SELECTED_CHECKBOX_NOT_CHECKED_SYMBOL, MENU_ICON_FONT, White);
					}
				}else{
					oled_write_char(MENU_SELECTED_SYMBOL, MENU_ICON_FONT, White);
				}
			}else{
				oled_set_cursor(0 + MENU_LEFT_OFFSET, ROW(i) + MENU_TOP_OFFSET + MENU_TEXT_SPACING*i);
				if(menu->items[index]->specNotSelected != 0 && !(menu->items[index]->flags & MENU_ITEM_IS_CHECKBOX)){
					oled_write_char(menu->items[index]->specNotSelected, MENU_ICON_FONT, White);
				}else if(menu->items[index]->flags & MENU_ITEM_IS_CHECKBOX){
					if(menu->items[index]->flags & MENU_ITEM_IS_CHECKED){
						oled_write_char(MENU_NOT_SELECTED_CHECKBOX_CHECKED_SYMBOL, MENU_ICON_FONT, White);
					}else{
						oled_write_char(MENU_NOT_SELECTED_CHECKBOX_NOT_CHECKED_SYMBOL, MENU_ICON_FONT, White);
					}
				}else{
					oled_write_char(MENU_NOT_SELECTED_SYMBOL, MENU_ICON_FONT, White);
				}
				
			}

			int posx = strlen(menu->items[index]->text[menuLanguage]) + 3;



			if(MENU_LINES > 1){
				if(menu->items[index]->flags & MENU_ITEM_IS_CHECKBOX){
					oled_set_cursor(COL(1) + MENU_LEFT_OFFSET + MENU_SELECTOR_SPACING, ROW(i) + MENU_TOP_OFFSET + MENU_TEXT_SPACING*i);
				}else oled_set_cursor(COL(1) + MENU_LEFT_OFFSET + MENU_SELECTOR_SPACING, ROW(i) + MENU_TOP_OFFSET + MENU_TEXT_SPACING*i);
				
				if(strlen(menu->items[index]->text[menuLanguage]) > 9){
					char tmp[10];
					if(menuItem == index){
						memcpy(tmp, menu->items[index]->text[menuLanguage]+menuScrollIndex, 9);
						memset(tmp+9, 0, strlen(menu->items[index]->text[menuLanguage])-9);
					}else{
						memcpy(tmp, menu->items[index]->text[menuLanguage], 9);
						memset(tmp+9, 0, strlen(menu->items[index]->text[menuLanguage])-9);
					}

					oled_write_string(tmp, MENU_FONT, White);
				}else oled_write_string(menu->items[index]->text[menuLanguage], MENU_FONT, White);

			}else{
				if(menu->items[index]->flags & MENU_ITEM_IS_CHECKBOX){
					oled_set_cursor(COL(0) + MENU_LEFT_OFFSET + MENU_SELECTOR_SPACING, ROW(i) + MENU_TOP_OFFSET + MENU_TEXT_SPACING*i);
				}else oled_set_cursor(COL(0) + MENU_LEFT_OFFSET + MENU_SELECTOR_SPACING, ROW(i) + MENU_TOP_OFFSET + MENU_TEXT_SPACING*i);

				if(strlen(menu->items[index]->text[menuLanguage]) > 9){
					char tmp[10];
					if(menuItem == index){
						memcpy(tmp, menu->items[index]->text[menuLanguage]+menuScrollIndex, 9);
						memset(tmp+9, 0, strlen(menu->items[index]->text[menuLanguage])-9);
					}else{
						memcpy(tmp, menu->items[index]->text[menuLanguage], 9);
						memset(tmp+9, 0, strlen(menu->items[index]->text[menuLanguage])-9);
					}

					oled_write_string(tmp, MENU_FONT, White);
				}else oled_write_string(menu->items[index]->text[menuLanguage], MENU_FONT, White);
			}
				if((menu->items[index]->flags & MENU_PARAMETER_MASK) == MENU_PARAMETER_IS_NUMBER){
					oled_set_cursor(COL(posx) + MENU_LEFT_OFFSET + MENU_SELECTOR_SPACING, ROW(i) + MENU_TOP_OFFSET + MENU_TEXT_SPACING*i);
					char buff[64];
					sprintf(buff, "%d", menu->items[index]->parameter);
					oled_write_string(buff, MENU_FONT, White);
				}

				if((menu->items[index]->flags & MENU_PARAMETER_MASK) == MENU_PARAMETER_IS_STRING){
					oled_set_cursor(COL(posx) + MENU_LEFT_OFFSET + MENU_SELECTOR_SPACING, ROW(i) + MENU_TOP_OFFSET + MENU_TEXT_SPACING*i);
					char buff[64];
					sprintf(buff, "%d", menu->items[index]->parameter);
					oled_write_string(buff, MENU_FONT, White);
				}

			i++;
		}
		  menuLastMenuItem = menuItem;
		  oled_update();
	}else if(menuMode == MENU_MENUMODE_SPLASH){
		(*menu_actual_splash)(menu_actual_splash_param);
		oled_update();
	}

}*/



/*

unsigned char menuLanguage;

unsigned char menuItem = 0;
unsigned char menuLastMenuItem = 255;

unsigned char menuCursorTopPos = 0;
unsigned char menuTopPos = 0;

int len = 0, i = 0;

uint8_t menuScrollIndex = 0, menuScrollPause = 0, menuScrollPauseDone = 0, menuScrollMax, menuForceUpdate = 0, menuMode = MENU_MENUMODE_NORMAL;

extern Scheduler sched_menu_scroll;
extern Scheduler sched_menu_update;*/
/*
Menu * menu_actual_menu;
void (*menu_actual_splash)(void *);
void * menu_actual_splash_param;
uint8_t menuActualSplashKeypress = 0;*/
/*
extern Menu menu_display, menu_settings, menu_set_display;*/
/*
extern void base_menu_set_current_source(void * m);
extern void base_play(uint8_t initiator, char * songname);
extern void base_stop(uint8_t initiator);
extern void base_req_songlist();


*/
/*
MenuItem menuitem_play = {{"Prehraj", "Play"}, (void*)&base_req_songlist, MENU_CALLBACK_IS_FUNCTION, 0, 0, 0};
MenuItem menuitem_stop = {{"Zastav", "Stop"}, (void*)&base_stop, MENU_CALLBACK_IS_FUNCTION, 0, 0, 0};
MenuItem menuitem_record = {{"Nahraj", "Record"}, 0, MENU_CALLBACK_IS_FUNCTION, 0, 0, 0};
MenuItem menuitem_organ_pwr = {{"Napajeni varhan", "Organ power"}, (void*)&base_menu_set_current_source, MENU_CALLBACK_IS_FUNCTION | MENU_ITEM_IS_CHECKBOX, 0, 0, 0};
MenuItem menuitem_display = {{"Ukazatel", "Display"}, (void*)&menu_display, MENU_CALLBACK_IS_SUBMENU, 0, 0, 0};
MenuItem menuitem_settings = {{"Nastaveni", "Settings"}, (void*)&menu_settings, MENU_CALLBACK_IS_SUBMENU, 0, 0, 0};

MenuItem menuitem_back = {{"Zpet", "Back"}, (void*)&menu_back, MENU_CALLBACK_IS_FUNCTION, 0, 37, 36};

MenuItem menuitem_paired_devices = {{"Sparovana zarizeni", "Paired devices"}, 0, MENU_CALLBACK_IS_SUBMENU, 0, 0, 0};
MenuItem menuitem_midi_loopback = {{"MIDI Loopback", "MIDI Loopback"}, 0, MENU_CALLBACK_IS_FUNCTION | MENU_ITEM_IS_CHECKBOX, 0, 0, 0};
MenuItem menuitem_midi_state = {{"MIDI Stav", "MIDI state"}, 0, MENU_CALLBACK_IS_FUNCTION, 0, 0, 0};
MenuItem menuitem_device_info = {{"Informace o zarizeni", "Device info"}, 0, MENU_CALLBACK_IS_FUNCTION, 0, 0, 0};

MenuItem menuitem_display_set = {{"Nastavit", "Set"}, (void*)&menu_set_display, MENU_CALLBACK_IS_SUBMENU, 0, 0, 0};
MenuItem menuitem_display_state = {{"Stav", "State"}, 0, MENU_CALLBACK_IS_FUNCTION, 0, 0, 0};

MenuItem menuitem_display_set_song = {{"Pisen", "Song"}, 0, MENU_CALLBACK_IS_FUNCTION, 0, 0, 0};
MenuItem menuitem_display_set_verse = {{"Sloku", "Verse"}, 0, MENU_CALLBACK_IS_FUNCTION, 0, 0, 0};
MenuItem menuitem_display_set_led = {{"LED", "LED"}, 0, MENU_CALLBACK_IS_FUNCTION, 0, 0, 0};
MenuItem menuitem_display_set_letter = {{"Pismeno", "Letter"}, 0, MENU_CALLBACK_IS_FUNCTION, 0, 0, 0};


Menu menu_main = {
	{"Hlavni menu","Main menu"},
    .items = {&menuitem_play, &menuitem_record, &menuitem_organ_pwr, &menuitem_display, &menuitem_settings, 0},
};

Menu menu_playing = {
	{"Prehrava se","Now playing"},
    .items = {&menuitem_stop, 0},
};


Menu menu_settings = {
    {"Nastaveni","Settings"},
	.parent = &menu_main,
    .items = { &menuitem_paired_devices, &menuitem_midi_loopback, &menuitem_midi_state, &menuitem_device_info, &menuitem_back, 0},
};

Menu menu_display = {
    {"Ukazatel","Display"},
	.parent = &menu_main,
    .items = { &menuitem_display_state, &menuitem_display_set, &menuitem_back, 0},
};

Menu menu_set_display = {
    {"Nastavit ukazatel","Set display"},
	.parent = &menu_display,
    .items = { &menuitem_display_set_song, &menuitem_display_set_verse, &menuitem_display_set_led, &menuitem_display_set_letter, &menuitem_back, 0},
};*/


/*
void menu_show(Menu *menu){

	//Setup pointer to shown menu (for other functions)
	menu_actual_menu = menu;

	//Zero out some variables
	len = 0;
	menuCursorTopPos = 0;
	menuTopPos = 0;

	//Count menu items
	MenuItem **iList = menu_actual_menu->items;
	for (; *iList != 0; ++iList){
		len++;
	}
	  
	if(menu_actual_menu->selectedIndex != -1){
	  // If the item is on the first screen
	  if(menu_actual_menu->selectedIndex < MENU_LINES){
		  menuItem = menu_actual_menu->selectedIndex;
		  menuCursorTopPos = menu_actual_menu->selectedIndex;
		  menuTopPos = 0;
	  }else{
		 //If the item is on the other screen
		  menuItem = menu_actual_menu->selectedIndex;
		  menuCursorTopPos = MENU_LINES - 1;
		  menuTopPos = menu_actual_menu->selectedIndex - menuCursorTopPos;
	  }
	}

}
*/


void menu_keypress(uint8_t key){
/*
	if(menuMode == MENU_MENUMODE_NORMAL){
		//Get actual menu from pointer
		Menu * menu = menu_actual_menu;

		//Move down event
		if(key == MENU_KEY_DOWN){
			if(menuItem != len-1){
				menuItem++;

				if(menuCursorTopPos >= MENU_LINES-1 || (menuCursorTopPos ==((MENU_LINES)/2) && ((len) - menuItem  ) > ((MENU_LINES-1)/2))){
					menuTopPos++;
				}else menuCursorTopPos++;

			}else{
				menuItem = 0;
				menuCursorTopPos = 0;
				menuTopPos = 0;
			}
		}

		//Move up event
		if(key == MENU_KEY_UP){
			if(menuItem != 0){
				menuItem--;

				if(menuCursorTopPos > 0 && !((menuCursorTopPos == MENU_LINES/2) && (menuItem >= MENU_LINES/2))){
					menuCursorTopPos--;
				}else menuTopPos--;
			}else{

				menuItem = len-1;

				if(len <= MENU_LINES){
					menuTopPos = 0;
				}else{
					menuTopPos = menuItem;
				}
				
				if(menuTopPos > len - MENU_LINES && len >= MENU_LINES){
					menuTopPos = len - MENU_LINES;
				}

				menuCursorTopPos = menuItem - menuTopPos;
			}
		}

		//Enter event
		if(key == MENU_KEY_ENTER){
			key = 0;
			menu->selectedIndex = menuItem;
			int flags = menu->items[menu->selectedIndex]->flags;

			//Checkbox without function callback
			if((menu->items[menu->selectedIndex]->callback == 0) && (flags & MENU_ITEM_IS_CHECKBOX)){
				menu->items[menu->selectedIndex]->flags ^= MENU_ITEM_IS_CHECKED;
				menuLastMenuItem = -1;
			}

			//Menu callback
			if(flags & MENU_CALLBACK_IS_SUBMENU && menu->items[menu->selectedIndex]->callback){
				menu_show((Menu*)menu->items[menu->selectedIndex]->callback);

				menuLastMenuItem = -1;
			}

			//Function callback
			if(flags & MENU_CALLBACK_IS_FUNCTION && menu->items[menu->selectedIndex]->callback){
				(*menu->items[menu->selectedIndex]->callback)(menu);

				menuLastMenuItem = -1;
			}

		}
	}else if(menuMode == MENU_MENUMODE_SPLASH){
		menuActualSplashKeypress = key;
	}*/
}

//Callback for back item click
void menu_back(void){
	/*menu_actual_menu->selectedIndex = 0;
	menu_show((Menu*)menu_actual_menu->parent);	*/		
}


void menu_show_splash(void (*callback)(void), void * param){
	/*menuMode = MENU_MENUMODE_SPLASH;
	menu_actual_splash = callback;
	menu_actual_splash_param = param;*/
}

void menu_hide_splash(){
	/*menuMode = MENU_MENUMODE_NORMAL;
	menuForceUpdate = 1;*/
}

//Funkce vykreslujici zapinaci obrazovku
void menu_start_splash(){
	//Vypisou se texty
	/*oled_set_cursor((128-(strlen(DEVICE_NAME))*11)/2,10);
	oled_write_string(DEVICE_NAME, Font_11x18, White);

	oled_set_cursor((128-(strlen(DEVICE_TYPE))*11)/2,30);
	oled_write_string(DEVICE_TYPE, Font_11x18, White);

	char * version = "Verze " FW_VERSION;
	oled_set_cursor((128-(strlen(version))*7)/2,50);
	oled_write_string(version, Font_7x10, White);*/

}

//Funkce pro vypsani chyby
void menu_error_splash(char * msg){
	/*oled_set_cursor((128-(strlen("Chyba!"))*11)/2, 1);
	oled_write_string("Chyba!", Font_11x18, White);

	oled_set_cursor((128-(strlen(msg))*7)/2, 20);
	oled_write_string(msg, Font_7x10, White);

	if(menuActualSplashKeypress == MENU_KEY_ENTER){
		menuActualSplashKeypress = 0;
		menu_hide_splash();
	}*/
}
