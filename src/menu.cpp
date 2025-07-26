#include "menu.hpp"
#include "scheduler.hpp"
#include "ble.hpp"
#include "comm.hpp"
#include "git.hpp"
#include "display.hpp"

#include <string>
#include <algorithm>


using namespace std;

Scheduler guiRenderScheduler(30, &GUI::render, Scheduler::PERIODICAL | Scheduler::ACTIVE | Scheduler::DISPATCH_ON_INCREMENT);
Scheduler menuScrollScheduler(2000, &GUI::scroll_callback, Scheduler::PERIODICAL | Scheduler::DISPATCH_ON_INCREMENT);

namespace GUI{
	//Variables global to the namespace
	bool forceRender = 0;
	int scrollIndex = 0;
	bool hadScrollPause = false;
	Language LANG = Language::CS;
	ScreenType screenType = ScreenType::SPLASH;

	//Menu declarations
	extern Menu menu_main;
	extern Menu menu_songlist;
	extern Menu menu_settings;
	extern Paragraph paragraph_firmwareInfo;

	//Menu callback declarations
	void toggleCallback(Item * itm);

	//Back button definition
	Item itm_back({{Language::EN, "Back"},{Language::CS, "Zpet"}}, &back, Oled::Icon::LEFT_ARROW_SEL, Oled::Icon::LEFT_ARROW_UNSEL);

	//Menu item definitions
	Item itm_play({{Language::EN, "Play"},{Language::CS, "Prehraj"}}, 
		[](Item * itm){	
			Communication::send(Communication::MUSIC::songlist, Communication::Command::Type::GET);
		}
	);

	Item itm_record({{Language::EN, "Record"},{Language::CS, "Nahraj"}}, [](Item * itm){
		Communication::send("set music record");
	 });

	Item itm_settings({{Language::EN, "Settings"},{Language::CS, "Nastaveni"}}, &menu_settings);

	Checkbox chck_power({{Language::EN, "Organ Power"},{Language::CS, "Napajeni Varhan"}},
		[](Item * itm){
			if(Base::CurrentSource::isEnabled()){
				Base::CurrentSource::disable();
				chck_power.setChecked(false);
			}else{
				Base::CurrentSource::enable();
				chck_power.setChecked(true);
			}
		}
	);

	Item itm_firmware({{Language::EN, "Firmware"},{Language::CS, "Firmware"}}, [](Item * itm){ display(&paragraph_firmwareInfo); });
	Item itm_display({{Language::EN, "Display"},{Language::CS, "Display"}}, [](Item * itm){ 
		string led;
		switch(Display::getLed()){
			
			case Display::LED::RED:
				led = "RED";
			break;
			case Display::LED::GREEN:
				led = "GREEN";
			break;
			case Display::LED::BLUE:
				led = "BLUE";
			break;
			case Display::LED::YELLOW:
				led = "YELLOW";
			break;
			case Display::LED::OFF:
				led = "OFF";
			break;
		}

		display(new Paragraph({
				"Connected: " + string((Display::getConnected() == true) ? "true" : "false"),
				"Song: " + to_string(Display::getSong()),
				"Verse: " + to_string(Display::getVerse()),
				"Letter: " + string(1, Display::getLetter()),
				"LED: " + led
				}, [](Paragraph * paragraph){ displayActiveMenu(); })
		);
		}
	);
	
	
	//Menu definitions
	Menu menu_main({{Language::EN, "Main menu"},{Language::CS, "Hlavni menu"}}, 
		{
			&itm_play,
			&itm_record,
			dynamic_cast<Item*>(&chck_power),
			&itm_settings
		}
	);

	Menu menu_songlist({{Language::EN, "Song list"},{Language::CS, "Seznam pisni"}},{&itm_back}, &menu_main);

	Menu menu_settings({{Language::EN, "Settings"},{Language::CS, "Nastaveni"}}, 
		{
			&itm_firmware,
			&itm_display,
			&itm_back
		}, &menu_main
	);

	Splash splash_welcome(Base::DEVICE_NAME, Base::DEVICE_TYPE, GIT::REVISION + " (" + GIT::DIRTY + ")", [](Splash * splash){ displayActiveMenu(); });
	
	Paragraph paragraph_firmwareInfo({
		"Build: " + GIT::REVISION,
	 	"Dirty: " + string((GIT::DIRTY == "dirty") ? "true" : "false"),
		"Date: " + GIT::DATE, "Time: " + GIT::TIME,
		"Branch: " + GIT::BRANCH},
		[](Paragraph * paragraph){ displayActiveMenu();}
	);


	//////////////////////////////////////////////////////////////////////////////////////////////////////////
	//////////////////////////////////////////////////////////////////////////////////////////////////////////
	//////////////////////////////////////////////////////////////////////////////////////////////////////////

	Menu * activeMenu = &menu_main;
	Splash * activeSplash = &splash_welcome;
	Paragraph * activeParagraph = nullptr;

	int Menu::getSelectedIndex(){
		return this->selectedIndex;
	}

	int Menu::setSelectedIndex(int index){
		if(index >= static_cast<int>(this->items.size()) || index < 0) return 0;
		this->lastSelectedIndex = this->selectedIndex;
		this->selectedIndex = index;
		return 1;
	}

	bool Menu::selectedIndexChanged(){
		if(this->selectedIndex != this->lastSelectedIndex){
			this->lastSelectedIndex = selectedIndex;
			return true;
		}
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
		if(this->selectedIndex == static_cast<int>(this->items.size())-1) return true;
		return false;
	}

	string Menu::getTitle(Language lang){
		return this->title.at(lang);
	}

	Item Menu::getItem(int index){
		if(this->items.empty()) return Item();
		return *this->items.at(index);
	}

	Item Menu::getSelectedItem(){
		if(this->items.empty()) return Item();
		return *this->items.at(this->getSelectedIndex());
	}

	Item Menu::getSelectedItem(int offset){
		if(this->items.empty()) return Item();
		return *this->items.at(this->getSelectedIndex() + offset);
	}

	Item * Menu::getSelectedItemPtr(){
		if(this->items.empty()) return new Item();
		return this->items.at(this->getSelectedIndex());
	}

	Item * Menu::getSelectedItemPtr(int offset){
		if(this->items.empty()) return new Item();
		return this->items.at(this->getSelectedIndex() + offset);
	}

	string Item::getTitle(Language lang){
		return this->title.at(lang);
	}

	Oled::Icon Item::getIcon(bool selected){
		return selected ? this->iconSelected : this->iconNotSelected;
	}

	Oled::Icon Checkbox::getIcon(bool selected){
		if(selected){
			return this->checked ? this->iconSelectedChecked : this->iconSelected;
		}else return this->checked ? this->iconNotSelectedChecked : this->iconNotSelected;
	}

	void Checkbox::setChecked(bool checked){
		this->checked = checked;
		renderForce();
	}

	Item::CallbackType Item::callbackType(){
		return this->clbType;
	}

	Item::ItemType Item::itemType(){
		return this->type;
	}
	
	bool Item::hasIcon(){
		return this->_hasIcon;
	}

	void display(Menu * menu){
		menuScrollScheduler.pause();
		menuScrollScheduler.setInterval(MENU_SCROLL_PAUSE);
		scrollIndex = 0;
		activeMenu = menu;
		screenType = ScreenType::MENU;
		activeMenu->setSelectedIndex(0);
		renderForce();
	}

	void display(Splash * splash){
		menuScrollScheduler.pause();
		menuScrollScheduler.setInterval(MENU_SCROLL_PAUSE);
		scrollIndex = 0;
		activeSplash = splash;
		screenType = ScreenType::SPLASH;
		renderForce();
	}

	void display(Paragraph * paragraph){
		menuScrollScheduler.pause();
		menuScrollScheduler.setInterval(MENU_SCROLL_PAUSE);
		scrollIndex = 0;
		activeParagraph = paragraph;
		screenType = ScreenType::PARAGRAPH;
		renderForce();
	}

	void displayActiveMenu(){
		screenType = ScreenType::MENU;
		renderForce();
	}

	void displayActiveSplash(){
		screenType =  ScreenType::SPLASH;
		renderForce();
	}

	void displayActiveParagraph(){
		screenType =  ScreenType::PARAGRAPH;
		renderForce();
	}

	void back(Item * itm){
		Menu * menu = activeMenu->parent;
		if(menu != nullptr)	display(menu);
	}

	void render(void){
		//If there is a splash screen to display, render it
		if(screenType == ScreenType::SPLASH){
			renderSplash();
			return;
		}else if(screenType == ScreenType::PARAGRAPH){
			renderParagraph();
			return;
		}else if(screenType == ScreenType::MENU){
			renderMenu();
			return;
		}
	}

	void Splash::setTitle(string title){
		this->title = title;
	}

	void Splash::setSubtitle(string subtitle){
		this->subtitle = subtitle;
	}

	void Splash::setComment(string comment){
		this->comment = comment;
	}

	string Splash::getTitle(){
		return this->title;
	}

	string Splash::getSubtitle(){
		return this->subtitle;
	}

	string Splash::getComment(){
		return this->comment;
	}

	Splash::CallbackType Splash::callbackType(){
		return this->clbType;
	}

	void Paragraph::setText(vector<string> text){
		this->text = text;
	}

	vector<string> Paragraph::getText(){
		return this->text;
	}

	Paragraph::CallbackType Paragraph::callbackType(){
		return this->clbType;
	}

	void renderForce(){
		forceRender = true;
	}

	void renderMenu(){
		//If there is nothing to update, return
		if(!activeMenu->selectedIndexChanged() && !forceRender) return;
		//Zero out the force render flag
		forceRender = 0;
		//Blank the oled
		Oled::fill(Oled::Color::BLACK);

		//Display connection icon
		if(BLE::isConnected()){
			Oled::setCursor({113,0});
			Oled::writeSymbol(48, Icon_7x10, Oled::Color::WHITE);
			Oled::setCursor({120,0});
			Oled::writeSymbol(49, Icon_7x10, Oled::Color::WHITE);
		}
		//If the menu has a title in selected language, display it
		if(activeMenu->hasTitle(LANG)){
			Oled::setCursor({0,0});
			Oled::writeString(activeMenu->getTitle(LANG), Font_7x10, Oled::Color::WHITE);
		}

		int itemCount = activeMenu->items.size() > 2 ? 2 : activeMenu->items.size();

		//If there are no items to display
		if(activeMenu->items.empty()){
			Oled::update();
			return;
		}

		//Display arrows indicating more items on top or bottom of the menu
		if(activeMenu->selectedFirstItem() && itemCount > 1){
			//Down arrow
			Oled::setCursor({117,41});
			Oled::writeSymbol(Oled::Icon::DOWN_ARROW, Icon_11x18, Oled::Color::WHITE);
		}else if(activeMenu->selectedLastItem() && itemCount > 1){
			//Up arrow
			Oled::setCursor({117,19});
			Oled::writeSymbol(Oled::Icon::UP_ARROW, Icon_11x18, Oled::Color::WHITE);
		}else if(itemCount > 1){
			//Down arrow
			Oled::setCursor({117,41});
			Oled::writeSymbol(Oled::Icon::DOWN_ARROW, Icon_11x18, Oled::Color::WHITE);
			//Up arrow
			Oled::setCursor({117,19});
			Oled::writeSymbol(Oled::Icon::UP_ARROW, Icon_11x18, Oled::Color::WHITE);
		}

		size_t titleLength = activeMenu->getSelectedItem().getTitle(LANG).length();
		//If the text is too long to display, enable scrolling
		int idx = activeMenu->getSelectedIndex();
		if(titleLength > 9 && !menuScrollScheduler.isActive()){
			scrollIndex = 0;
			hadScrollPause = false;
			menuScrollScheduler.pause();
			menuScrollScheduler.setInterval(MENU_SCROLL_PAUSE);
			menuScrollScheduler.reset();
			menuScrollScheduler.resume();
		}else if (titleLength <= 9) menuScrollScheduler.pause();

		int indexShift = 0;

		if(activeMenu->selectedFirstItem()){
			indexShift = 0;
		}else if(activeMenu->selectedLastItem()){
			indexShift = 1;
		}
		
		
		for(int i = 0; i < itemCount; i++){
			//Draw the icon next to menu item
			if(activeMenu->getSelectedItem(i - indexShift).hasIcon()){
				Oled::setCursor({0 + MENU_LEFT_OFFSET, ROW(i) + MENU_TOP_OFFSET + MENU_TEXT_SPACING*i});
				if(activeMenu->getSelectedItem(i - indexShift).itemType() == Item::ItemType::CHECKBOX){
					Checkbox * chck = dynamic_cast<Checkbox*>(activeMenu->getSelectedItemPtr(i - indexShift));
					Oled::writeSymbol(chck->getIcon(i == indexShift), MENU_ICON_FONT, Oled::Color::WHITE);
				}else Oled::writeSymbol(activeMenu->getSelectedItem(i - indexShift).getIcon(i == indexShift), MENU_ICON_FONT, Oled::Color::WHITE);
				
			}

			//Draw the menu item
			Oled::setCursor({COL(1) + MENU_LEFT_OFFSET + MENU_SELECTOR_SPACING, ROW(i) + MENU_TOP_OFFSET + MENU_TEXT_SPACING*i});
			//Title reduced to 9 characters to fit on screen
			string reducedTitle = activeMenu->getSelectedItem(i - indexShift).getTitle(LANG).substr(i == indexShift ? scrollIndex : 0, 9);
			Oled::writeString(reducedTitle, MENU_FONT, Oled::Color::WHITE);
			
		}

		Oled::update();
	}

	void renderSplash(){
		Oled::fill(Oled::Color::BLACK);

		Oled::setCursor({64 - (activeSplash->getTitle().substr(0,11).length() * 11)/2, 0});
		Oled::writeString(activeSplash->getTitle().substr(0,11), Font_11x18, Oled::Color::WHITE);

		//If the text is too long to display, enable scrolling
		if(activeSplash->getSubtitle().length() > 12 && !menuScrollScheduler.isActive()){
			scrollIndex = 0;
			hadScrollPause = false;
			menuScrollScheduler.pause();
			menuScrollScheduler.setInterval(MENU_SCROLL_PAUSE);
			menuScrollScheduler.reset();
			menuScrollScheduler.resume();
		}else if (activeSplash->getSubtitle().length() < 12) menuScrollScheduler.pause();

		string reducedTitle = activeSplash->getSubtitle().substr(scrollIndex, 11);
		Oled::setCursor({64 - (reducedTitle.length() * 11)/2, 25});
		Oled::writeString(reducedTitle, Font_11x18, Oled::Color::WHITE);

		Oled::setCursor({64 - (activeSplash->getComment().substr(0,18).length() * 7)/2, 53});
		Oled::writeString(activeSplash->getComment().substr(0,18), Font_7x10, Oled::Color::WHITE);

		Oled::wakeupCallback();
		Oled::update();
	}

	void renderParagraph(){
		Oled::fill(Oled::Color::BLACK);

		int ycord = 0;
		for(string line : activeParagraph->getText()){
			Oled::setCursor({2, ycord});
			Oled::writeString(line.substr(0,18), Font_7x10, Oled::Color::WHITE);
			ycord += 12;
		}

		Oled::wakeupCallback();
		Oled::update();
	}

	void scroll_callback(){

		int limit = 9;
		size_t titleLength = activeMenu->getSelectedItem().getTitle(LANG).length();

		if(activeMenu->items.empty() && !(screenType == ScreenType::SPLASH)) return;

		if(screenType == ScreenType::SPLASH){
			limit = 12;
			titleLength = activeSplash->getSubtitle().length();
		}

		
		if(hadScrollPause){
			if(scrollIndex < titleLength - limit - 1){
				scrollIndex++;
			}else if(scrollIndex == titleLength - limit - 1){
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
	
	void keypress(UserInput::Key key){

		if(screenType == ScreenType::SPLASH){
			if(activeSplash->callbackType() == Splash::CallbackType::FUNCTION && key == UserInput::Key::ENTER){
				menuScrollScheduler.pause();
				menuScrollScheduler.setInterval(MENU_SCROLL_PAUSE);
				scrollIndex = 0;
				(*activeSplash->callback)(activeSplash);
			}

			return;
		}else if(screenType == ScreenType::PARAGRAPH){
			if(activeParagraph->callbackType() == Paragraph::CallbackType::FUNCTION && key == UserInput::Key::ENTER){
				menuScrollScheduler.pause();
				menuScrollScheduler.setInterval(MENU_SCROLL_PAUSE);
				scrollIndex = 0;
				(*activeParagraph->callback)(activeParagraph);
			}

			return;
		}

		if(Oled::isSleeping()){
			Oled::wakeupCallback();
			forceRender = true;
			return;
		}

		Oled::wakeupCallback();
		forceRender = true;
		
		//Reset scrolling		
		if(key == UserInput::Key::UP && !activeMenu->selectedFirstItem()){
			menuScrollScheduler.pause();
			menuScrollScheduler.setInterval(MENU_SCROLL_PAUSE);
			scrollIndex = 0;
			activeMenu->setSelectedIndex(activeMenu->getSelectedIndex() - 1);
		}else if(key == UserInput::Key::DOWN && !activeMenu->selectedLastItem()){
			menuScrollScheduler.pause();
			menuScrollScheduler.setInterval(MENU_SCROLL_PAUSE);
			scrollIndex = 0;
			activeMenu->setSelectedIndex(activeMenu->getSelectedIndex() + 1);
		}else if(key == UserInput::Key::ENTER){
			if(activeMenu->getSelectedItem().callbackType() == Item::CallbackType::FUNCTION){
				menuScrollScheduler.pause();
				menuScrollScheduler.setInterval(MENU_SCROLL_PAUSE);
				scrollIndex = 0;
				Item selItm = activeMenu->getSelectedItem();
				(*activeMenu->getSelectedItem().callback)(&selItm);
			}else if(activeMenu->getSelectedItem().callbackType() == Item::CallbackType::SUBMENU){
				menuScrollScheduler.pause();
				menuScrollScheduler.setInterval(MENU_SCROLL_PAUSE);
				scrollIndex = 0;
				GUI::display(activeMenu->getSelectedItem().child);
			}
		}

		
	}
}
