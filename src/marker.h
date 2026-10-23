#pragma once
#include <cstdint>
namespace eye {
// Restores a session's original native debug flag when our override is released.
struct MarkerOverride {
    bool held=false;
    uint8_t original=0;
    bool Set(uint8_t& flag,bool enabled) {
        if(flag>1)return false;
        if(enabled){if(!held){original=flag;held=true;}flag=1;}
        else if(held){flag=original;held=false;}
        return true;
    }
};
}
