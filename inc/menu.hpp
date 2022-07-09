#ifndef MENU_H
#define MENU_H

#include "oled.hpp"
#include "oled_fonts.hpp"
#include <string>
#include <vector>
#include <map>

using namespace std;

#define MENU_MENUMODE_NORMAL	0
#define MENU_MENUMODE_SPLASH	1

// Set rows/cols based on your font (for graphical displays)
#define ROW(x) ((x)*18)
#define COL(y) ((y)*11)

#define MENU_TOP_OFFSET			21
#define MENU_LEFT_OFFSET		0
#define MENU_TEXT_SPACING		3

// Number of items on one screen
// Not including title
#define MENU_LINES 2

#define MENU_ICON_FONT Icon_11x18

#define MENU_SCROLL_PAUSE	1000

// How many spaces is between arrow symbol and menu item
// useful to set zero on smaller displays
#define MENU_SELECTOR_SPACING	 3

// 0-3 bits
#define MENU_PARAMETER_MASK 0x0F
#define MENU_PARAMETER_IS_NUMBER 1
#define MENU_PARAMETER_IS_STRING 2

// 4. bit checkbox bit
#define MENU_ITEM_IS_CHECKBOX	0x10
// 5bit
#define MENU_ITEM_IS_CHECKED	0x20

// 6.bit - submenu bit
#define MENU_CALLBACK_IS_SUBMENU	0x40

// 7bit - callback bit
#define MENU_CALLBACK_IS_FUNCTION 0x80

#define MENU_FONT		Font_11x18

namespace UserInput{
	enum class Key{
		UP,
		DOWN,
		ENTER
	};
}


namespace GUI{

	//Language selection
	enum class Language{
		EN ,
		CS
	};

	//Menu Item class
	class Item{
		private:
			int flags = 0;
			int parameter = 0;
			
		public:
			enum class CallbackType{
				NONE,
				SUBMENU,
				FUNCTION
			};

			map<Language, string> title;
			void (*callback)(void *) = NULL;
			CallbackType callbackType;

			Oled::Icon iconSelected;
			Oled::Icon iconNotSelected;

			class Menu * parent = NULL;
			class Menu * child = NULL;
			Item(const map<Language, string>  title, CallbackType callbackType = CallbackType::NONE, Oled::Icon iconSelected = Oled::Icon::DOT_SEL, Oled::Icon iconNotSelected = Oled::Icon::DOT_UNSEL);

	};

	class Checkbox : public Item{
		private:
			bool checked;
		public:
			Checkbox(const map<Language, string> title, CallbackType callbackType = CallbackType::NONE, bool checked = false);
	};

	class Back : public Item{
		public:
			Oled::Icon iconSelected = Oled::Icon::LEFT_ARROW_SEL;
			Oled::Icon iconNotSelected = Oled::Icon::LEFT_ARROW_UNSEL;
			Back(const map<Language, string>  title, CallbackType callbackType = CallbackType::SUBMENU);
	};


	class Menu{
		private:
			int selectedIndex = 0;
			int lastSelectedIndex = -1;
		public:
			map<Language, string> title;
			vector<Item> items;
			Menu(const map<Language, string> title = {}, vector<Item> itms = {});

			int setSelectedIndex(int index);
			int getSelectedIndex();
			bool selectedIndexChanged();
			bool hasTitle(Language lan);
			bool selectedFirstItem();
			bool selectedLastItem();
	};

	void display(Menu & menu);
	void render(void);
	void keypress(UserInput::Key key);
	void back(void);
	void scroll_callback(void);

	void show_splash(void (*callback)(void), void * param);
	void hide_splash(void);

	void start_splash(void);
	void error_splash(char * msg);

}

#endif