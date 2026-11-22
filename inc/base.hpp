
#ifndef BASE_H
#define BASE_H

#include <string>
#include <cstdint>
#include "../src/logic/organ_source.hpp"
#include <stm32g431xx.h>
#include <stmcpp/error.hpp>

namespace base{
	
    using namespace std;

    static string DEVICE_NAME = "MIDIControl";
    static string DEVICE_TYPE = "BASE"; // device name, do not translate
    static string FW_VERSION = "2.0.0";

    void initClock(void);

    namespace current{
		
        bool isEnabled();
        void enable();
        void disable();
        void set(bool value);
        // Who switched the power on (A5, INFO ORGAN_SRC): call BEFORE enable()/set(true); disable() forgets it
        void setSource(organsrc::Source src);
        uint8_t sourceCode(); // 0 off, 1 PC, 2 menu, 3 unknown
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

