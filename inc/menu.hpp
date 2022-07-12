#ifndef MENU_H
#define MENU_H

#include "oled.hpp"
#include "oled_fonts.hpp"
#include <string>
#include <vector>
#include <map>
#include <memory>

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
		public:
			enum class ItemType{
				ITEM,
				CHECKBOX,
				BACK
			};

			enum class CallbackType{
				NONE,
				SUBMENU,
				FUNCTION
			};

		private:
			//int flags = 0;
			//int parameter = 0;
			
			map<Language, string> title;
			ItemType type;
			CallbackType clbType;
			Oled::Icon iconSelected;
			Oled::Icon iconNotSelected;
			
		public:
			
			void (*callback)(void *) = NULL;

			class Menu * parent = NULL;
			class Menu * child = NULL;
			Item(map<Language, string> title, CallbackType callbackType = CallbackType::NONE, ItemType type = ItemType::ITEM, Oled::Icon iconSelected = Oled::Icon::DOT_SEL, Oled::Icon iconNotSelected = Oled::Icon::DOT_UNSEL) : title(title), type(type), clbType(callbackType), iconSelected(iconSelected), iconNotSelected(iconNotSelected){};
			Item(const Item&) = delete;
			Item(Item&&) = default;
			string getTitle(Language lang);
			Oled::Icon getIcon(bool selected);
			CallbackType callbackType(){return clbType;};
	};

	class Checkbox : public Item{
		private:
			bool checked;
			Oled::Icon iconSelectedChecked;
			Oled::Icon iconNotSelectedChecked;
		public:
			Checkbox(map<Language, string> title, CallbackType callbackType = CallbackType::NONE, Oled::Icon iconSelected = Oled::Icon::CHECKBOX_SEL_UNCH, Oled::Icon iconNotSelected = Oled::Icon::CHECKBOX_UNSEL_UNCH, bool checked = false, Oled::Icon iconSelectedChecked = Oled::Icon::CHECKBOX_SEL_CHCK, Oled::Icon iconNotSelectedChecked = Oled::Icon::CHECKBOX_UNSEL_CHCK) : Item(title, callbackType, ItemType::CHECKBOX, iconSelected, iconNotSelected), checked(checked), iconSelectedChecked(iconSelectedChecked), iconNotSelectedChecked(iconNotSelectedChecked){};
			Checkbox(const Checkbox&) = delete;
			Checkbox(Checkbox&&) = default;
	};

	class Back : public Item{
		public:
			Oled::Icon iconSelected = Oled::Icon::LEFT_ARROW_SEL;
			Oled::Icon iconNotSelected = Oled::Icon::LEFT_ARROW_UNSEL;
			Back(map<Language, string>  title, CallbackType callbackType = CallbackType::SUBMENU) : Item(title, callbackType, ItemType::BACK, Oled::Icon::LEFT_ARROW_SEL, Oled::Icon::LEFT_ARROW_UNSEL){};
			Back(const Back&) = delete;
			Back(Back&&) = default;
	};


	class Menu{
		private:
			int selectedIndex = 0;
			int lastSelectedIndex = -1;
		public:
			map<Language, string> title;
			vector<unique_ptr<Item>> items;
			Menu(const map<Language, string> title, vector<unique_ptr<Item>> items) : title(title), items(items){};

			int setSelectedIndex(int index);
			int getSelectedIndex();
			bool selectedIndexChanged();
			bool hasTitle(Language lan);
			bool selectedFirstItem();
			bool selectedLastItem();
			Item getItem(int index);
			Item getSelectedItem();
			Item getSelectedItem(int offset);
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