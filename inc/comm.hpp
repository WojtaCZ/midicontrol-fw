
#ifndef COMM_H
#define COMM_H

#include <string>
#include <functional>

using namespace std;

namespace Communication{
    class Command{
        public:
            enum class Type{
                GET,
                SET,
                RESP,
                UNDEFINED
            };
        private:
            //Command which summons the handler
            string cmd;
            //Data sfter the command
            string data; 
            //Callback function
            void (*_callback)(string data, Type type);
        public:
            //Constructor
            Command(string command, void (*callback)(string data, Type type)) : cmd(command), _callback(callback){ };
            void callback(string data, Type type);
            string getCommand();
            void setCommand(string command);

    };

    void decode(string data);
    void send(string data);
    void send(char c);

}

//Wrapper to call from C files
extern "C" void comm_decode(char * data, int len);

#endif
