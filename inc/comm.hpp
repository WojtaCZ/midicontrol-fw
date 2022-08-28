
#ifndef COMM_H
#define COMM_H

#include <string>
#include <functional>

using namespace std;

namespace Communication{

    namespace Error{
        constexpr auto unsupported =    "unsupported command";
        constexpr auto unknown =        "unknown command";
        constexpr auto wrong_input =    "wrong input";
    }

    class Command{
        public:
            enum class Type{
                GET,
                SET,
                UNDEFINED
            };
        private:
            //Command which summons the handler
            string cmd;
            //Data sfter the command
            string data; 
            //Callback function when the command is received
            void (*receiveHandler)(string data, Type type);
            //Callback function when a response to the command is received
            void (*responseHandler)(string data);
            //Does this command await a response?
            bool awaitResponse, receivedResponse;
        public:
            //Constructor
            Command(string command, void (*clb_recv)(string data, Type type), void (*clb_resp)(string data)) : cmd(command), receiveHandler(clb_recv), responseHandler(clb_resp), awaitResponse(true), receivedResponse(false){ };
            Command(string command, void (*clb_recv)(string data, Type type)) : cmd(command), receiveHandler(clb_recv), responseHandler(nullptr), awaitResponse(false), receivedResponse(false){ };
            void recvHandler(string data, Type type);
            void respHandler(string data);
            string getCommand();
            void setCommand(string command);
            bool requiresResponse();
            bool gotResponse();
            bool gotResponse(bool got);

    };

    void decode(string data);
    void send(string data);
    void send(char c);
    void send(Command cmd, Command::Type type);
    void send(Command cmd, Command::Type type, string data);
    void error();
    void error(string type);
    void ok();
    void ok(string type);
    void commTimeoutCallback();

    namespace MUSIC{
        extern Command songlist;
        extern Command play;
        extern Command record;
        extern Command stop;
    }

}

//Wrapper to call from C files
extern "C" void comm_decode(char * data, int len);

#endif
