
#ifndef BASE_H
#define BASE_H

#include <string>
#include <stm32g431xx.h>
#include <stmcpp/error.hpp>

namespace base{
	
    using namespace std;

    static string DEVICE_NAME = "MIDIControl";
    static string DEVICE_TYPE = "BASE";
    static string FW_VERSION = "2.0.0";

    void initClock(void);
	void dfuCheck();
	void wdtStart();

    namespace current{
		
        bool isEnabled();
        void enable();
        void disable();
        void toggle(void);
        void set(bool value);
    };

    namespace Encoder{
        void process();
    };


    enum class error {
        hse_timeout,
        hsi_timeout,
        hsi48_timeout,
        csi_timeout,
        flash_latency_timeout,
        pll_timeout,
        domain_source_switch_timeout,
        clock_compensation_timeout,
        clock_mux_timeout,
        other
    };

    static stmcpp::error::handler<error, "base"> errorHandler;

}

#endif

