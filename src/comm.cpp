#include "comm.hpp"
#include "scheduler.hpp"
#include "base.hpp"
#include "menu.hpp"
#include "display.hpp"
#include "ble.hpp"

#include <cstdlib>
#include <tinyusb/src/device/usbd.h>
#include <tinyusb/src/class/cdc/cdc_device.h>



Scheduler commTimeoutScheduler(2000, &Communication::commTimeoutCallback, Scheduler::DISPATCH_ON_INCREMENT);

namespace Communication{
	GUI::Menu menu_songlist({{GUI::Language::EN, "Song list"},{GUI::Language::CS, "Seznam pisni"}},{}, &(GUI::menu_main));

	//Command definitions
	namespace MUSIC{
		Command songlist = Command("music songlist",  [](string data, Command::Type type){
			if(type == Command::Type::GET){
				std::string s = "get music songlist\r\n";
				tud_cdc_write_str(s.c_str());
			}else if(type == Command::Type::SET) error(Error::unsupported);

		}, [](string data){
			menu_songlist.items.clear();
			size_t pos = 0;
			string token;
			while ((pos = data.find(";")) != string::npos) {
				token = data.substr(0, pos);

				menu_songlist.items.push_back(new GUI::Item({{GUI::Language::EN, token},{GUI::Language::CS, token}},
					[](GUI::Item * itm){
						Communication::send(MUSIC::play, Command::Type::SET, itm->getTitle(GUI::Language::CS));
						GUI::display(new GUI::Splash("Prehravam",  itm->getTitle(GUI::Language::CS), "Zastavite stiskem", [](GUI::Splash * splash){Communication::send(MUSIC::stop, Command::Type::SET); GUI::display(&GUI::menu_main);}));
					}
				));
				data.erase(0, pos + 1);
			}

			menu_songlist.items.push_back(&(GUI::itm_back));
			menu_songlist.setSelectedIndex(0);
			GUI::display(&menu_songlist);
		});



		Command play = Command("music play", [](string data, Command::Type type){
			if(type == Command::Type::GET){
			
			}else if(type == Command::Type::SET){
				send(play, Command::Type::SET, data);	
			}

		});

		Command bind_play = Command("music bind play", [](string data, Command::Type type){
			if(type == Command::Type::GET){
			
			}else if(type == Command::Type::SET){
				std::string s = "set music bind play " + data + "\r\n";
				tud_cdc_write_str(s.c_str());	
			}

		});

		Command record = Command("music record", [](string data, Command::Type type){
			if(type == Command::Type::GET){
			
			}else if(type == Command::Type::SET){
				GUI::display(new GUI::Splash("Nahravam",  data, "Zastavite stiskem", [](GUI::Splash * splash){Communication::send(MUSIC::stop, Command::Type::SET); GUI::display(&GUI::menu_main);}));
				//Send over state of the display when recording is started
				display::sendToMIDI();
			}

		});

		Command stop = Command("music stop", [](string data, Command::Type type){
			if(type == Command::Type::GET){
			
			}else if(type == Command::Type::SET){
				std::string s = "set music stop\r\n";
				tud_cdc_write_str(s.c_str());	
			}

		});

	}

	namespace DISPLAY{
		Command status = Command("display status", [](string data, Command::Type type){
			if(type == Command::Type::GET){
				send(display::getConnected() ? "connected" : "disconnected");
			}else if(type == Command::Type::SET){
				error(Error::unsupported);
			}else error(Error::unknown);
		});
		Command song = Command("display song", [](string data, Command::Type type){
			if(type == Command::Type::GET){
				send(to_string(display::getSong()));
			}else if(type == Command::Type::SET){
				if(data == "none"){
					display::setSong(0, false);
					ok();
				}else{
					if(!data.empty() && data.find_first_not_of("0123456789") == std::string::npos){
						display::setSong(stoi(data), true);
						ok();
					}else{
						error(Error::wrong_input);
					}
				}
			}else error(Error::unknown);
		});
		Command verse = Command("display verse", [](string data, Command::Type type){
			if(type == Command::Type::GET){
				send(to_string(display::getVerse()));
			}else if(type == Command::Type::SET){
				if(data == "none"){
					display::setVerse(0, false);
					ok();
				}else{
					if(!data.empty() && data.find_first_not_of("0123456789") == std::string::npos){
						display::setVerse(stoi(data), true);
						ok();
					}else{
						error(Error::wrong_input);
					}
				}
			}else error(Error::unknown);
		});
		Command letter = Command("display letter", [](string data, Command::Type type){
			if(type == Command::Type::GET){
				send(display::getLetter());
			}else if(type == Command::Type::SET){
				if(data == "none"){
					display::setLetter('A', false);
					ok();
				}else{
					if(!data.empty() && data.find_first_not_of("ABCD") == std::string::npos){
						display::setLetter(data.at(0), true);
						ok();
					}else{
						error(Error::wrong_input);
					}
				}
			}else error(Error::unknown);
		});

		Command led = Command("display led", [](string data, Command::Type type){
			if(type == Command::Type::GET){
				switch(display::getLed()){
					case display::ledColor::RED:
						send("red");
					break;

					case display::ledColor::GREEN:
						send("green");
					break;

					case display::ledColor::BLUE:
						send("blue");
					break;

					case display::ledColor::YELLOW:
						send("yellow");
					break;

					default:
						send("off");
					break;
				}
			}else if(type == Command::Type::SET){
				if(data == "red"){
					display::setLed(display::ledColor::RED);
					ok();
				}else if(data == "green"){
					display::setLed(display::ledColor::GREEN);
					ok();
				}else if(data == "blue"){
					display::setLed(display::ledColor::BLUE);
					ok();
				}else if(data == "yellow"){
					display::setLed(display::ledColor::YELLOW);
					ok();
				}else if(data == "off"){
					display::setLed(display::ledColor::OFF);
					ok();
				}else error(Error::wrong_input);
			}else error(Error::unknown);
		});
	}

	namespace BASE{

		Command current = Command("base current", [](string data, Command::Type type){
			if(type == Command::Type::SET){
				if(data == "on"){
					Base::CurrentSource::enable();
					GUI::chck_power.setChecked(true);
					ok();
				}else if(data == "off"){
					Base::CurrentSource::disable();
					GUI::chck_power.setChecked(false);
					ok();
				}
			}else if(type == Command::Type::GET){
				if(Base::CurrentSource::isEnabled()){
					Communication::send("on");
				}else{
					Communication::send("off");
				}
			}
		});
	}

	//Create a list of commands
	vector<Command> commands = {
		MUSIC::songlist,
		MUSIC::play,
		MUSIC::bind_play,
		MUSIC::record,
		MUSIC::stop,
		DISPLAY::status,
		DISPLAY::song,
		DISPLAY::verse,
		DISPLAY::letter,
		DISPLAY::led,
		BASE::current
	};


	bool awaitResponse = false;
	string dataBuffer;

	void commTimeoutCallback(){
		commTimeoutScheduler.pause();
		commTimeoutScheduler.reset();
		awaitResponse = false;
		BLE::clearBuffer();
	}

	//Call the receive callback function
	void Command::recvHandler(string data, Type type){
		(this->receiveHandler)(data, type);
	}

	//Call the response callback function
	void Command::respHandler(string data){
		(this->responseHandler)(data);
	}

	string Command::getCommand(){
		return this->cmd;
	}

	void Command::setCommand(string command){
		this->cmd = command;
	}

	bool Command::requiresResponse(){
		return this->awaitResponse;
	}

	bool Command::gotResponse(){
		return this->receivedResponse;
	}

	bool Command::gotResponse(bool got){
		this->receivedResponse = got;
		return this->receivedResponse;
	}

	void decode(string data){
		if(awaitResponse){
			for(auto command : commands){
			   if(command.requiresResponse() && !command.gotResponse()){
					//If we havent received the whole buffer yet
					if(data.find("\r\n") == string::npos){
						dataBuffer += data;
					}else{
						dataBuffer += data;
						commTimeoutScheduler.pause();
						commTimeoutScheduler.reset();
						command.gotResponse(true);
						awaitResponse = false;
						command.respHandler(dataBuffer);
						dataBuffer.clear();
					}
					return;
			   }
			}
		}else{
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
						command.recvHandler(data.substr(index + command.getCommand().length()+1, pos), type);
					}
					return;
				}
			}
		}
	}

	void send(string data){
		data += "\r\n";
		BLE::send(data);
		tud_cdc_write_str(data.c_str());
		
	}

	void send(Command cmd, Command::Type type){
	   send(cmd, type, "");
	}

	void send(Command cmd, Command::Type type, string data){
		string typeString;
		if(type == Command::Type::GET){
			awaitResponse = cmd.requiresResponse();
			cmd.gotResponse(false);
			typeString = "get";
			commTimeoutScheduler.reset();
			commTimeoutScheduler.resume();
		}else if(type == Command::Type::SET){
			typeString = "set";
		}

		string dataStr = typeString + " " + cmd.getCommand() + (data.empty() ? "" : " " + data) + "\r\n";
		BLE::send(dataStr);
		tud_cdc_write_str(dataStr.c_str());
		
	}

	void send(char c){
		string data;
		data = string(1,c) + "\r\n";
		BLE::send(data);
		tud_cdc_write_str(data.c_str());
		
	}

	void error(){
		send("error");
	}

	void error(string type){
		send("error " + type);
	}

	void ok(){
		send("ok");
	}

	void ok(string type){
		send("ok " + type);
	}
}

//Wrapper to call from C files
extern "C" void comm_decode(char * data, int len){
	Communication::decode(string(data, len));
}