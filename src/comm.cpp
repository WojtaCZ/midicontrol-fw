#include "comm.hpp"
#include "usb.h"
#include "scheduler.hpp"
#include "base.hpp"
#include "menu.hpp"
#include "display.hpp"

#include <cstdlib>


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

		Command status = Command("display status", &statusCallback);
		Command song = Command("display song", &songCallback);
		Command verse = Command("display verse", &verseCallback);
		Command letter = Command("display letter", &letterCallback);
		Command led = Command("display led", &ledCallback);
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
}

//Wrapper to call from C files
extern "C" void comm_decode(char * data, int len){
	string s = string(data, len);

	Communication::decode(s);
}