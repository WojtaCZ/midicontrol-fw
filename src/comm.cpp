#include "comm.hpp"
#include "scheduler.hpp"
#include "base.hpp"
#include "menu.hpp"
#include "display.hpp"

#include <cstdlib>

extern "C" uint32_t usb_cdc_tx(void *buf, int len);

namespace Communication{
	

	//Command definitions
	namespace MUSIC{
		void songlistCallback(string data, Command::Type type){}
		void playCallback(string data, Command::Type type){}
		void recordCallback(string data, Command::Type type){}
		void stopCallback(string data, Command::Type type){}

		Command songlist = Command("music songlist", &songlistCallback);
		Command play = Command("music play", &playCallback);
		Command record = Command("music record", &recordCallback);
		Command stop = Command("music stop", &stopCallback);

	}

	namespace DISPLAY{
		void statusCallback(string data, Command::Type type){}
		void songCallback(string data, Command::Type type){}
		void verseCallback(string data, Command::Type type){}
		void letterCallback(string data, Command::Type type){}
		void ledCallback(string data, Command::Type type){}

		Command status = Command("display status", [](string data, Command::Type type){
			if(type == Command::Type::GET){
				send(Display::getConnected() ? "connected" : "disconnected");
			}
		});
		Command song = Command("display song", [](string data, Command::Type type){
			if(type == Command::Type::GET){
				send(to_string(Display::getSong()));
			}else if(type == Command::Type::SET) Display::setSong(stoi(data), data == "" ? false : true);
		});
		Command verse = Command("display verse", [](string data, Command::Type type){
			if(type == Command::Type::GET){
				send(to_string(Display::getVerse()));
			}else if(type == Command::Type::SET) Display::setVerse(stoi(data), data == "" ? false : true);
		});
		Command letter = Command("display letter", [](string data, Command::Type type){
			if(type == Command::Type::GET){
				send(Display::getLetter());
			}else if(type == Command::Type::SET) Display::setLetter(data.at(0), data == "" ? false : true);
		});
		Command led = Command("display led", [](string data, Command::Type type){
			if(type == Command::Type::GET){
				switch(Display::getLed()){
					case Display::LED::RED:
						send("red");
					break;

					case Display::LED::GREEN:
						send("green");
					break;

					case Display::LED::BLUE:
						send("blue");
					break;

					case Display::LED::YELLOW:
						send("yellow");
					break;

					default:
						send("off");
					break;
				}
			}else if(type == Command::Type::SET){
				if(data == "red") Display::setLed(Display::LED::RED);
				if(data == "green") Display::setLed(Display::LED::GREEN);
				if(data == "blue") Display::setLed(Display::LED::BLUE);
				if(data == "yellow") Display::setLed(Display::LED::YELLOW);
				if(data == "off") Display::setLed(Display::LED::OFF);
			}
		});
	}

	namespace BASE{
		void currentCallback(string data, Command::Type type){
			if(type == Command::Type::SET){
				if(data == "on"){
					Base::CurrentSource::enable();
				}else if(data == "off"){
					Base::CurrentSource::disable();
				}
			}else if(type == Command::Type::GET){

			}
		}

		Command current = Command("base current", &currentCallback);
	}

	//Create a list of commands
	vector<Command> commands = {
		MUSIC::songlist,
		MUSIC::play,
		MUSIC::record,
		MUSIC::stop,
		DISPLAY::status,
		DISPLAY::song,
		DISPLAY::verse,
		DISPLAY::letter,
		DISPLAY::led,
		BASE::current
	};

	//Call the callback function
	void Command::callback(string data, Type type){
		(this->_callback)(data, type);
	}

	string Command::getCommand(){
		return this->cmd;
	}

	void Command::setCommand(string command){
		this->cmd = command;
	}

	void decode(string data){
		Command::Type type;
		for(auto command : commands){
			size_t index = data.find(command.getCommand());
			if(index != string::npos){
				if(index > 3){
					if(data.substr(index - 4, 3) == "get"){
						type = Command::Type::GET;
					}else if((data.substr(index - 4, 3) == "set")){
						type = Command::Type::SET;
					}else{
						type = Command::Type::UNDEFINED;
					}
					int pos = data.find("\r\n", index) - (command.getCommand().length()+index+1);
					command.callback(data.substr(index + command.getCommand().length()+1, pos), type);
				}
				return;
			}
		}
	}

	void send(string data){
		data += "\n\r";
		usb_cdc_tx((void *)data.c_str(), data.length());
	}

	void send(char c){
		string data;
		data = c + "\n\r";
		usb_cdc_tx((void *)data.c_str(), data.length());
	}
}

//Wrapper to call from C files
extern "C" void comm_decode(char * data, int len){
	Communication::decode(string(data, len));
}