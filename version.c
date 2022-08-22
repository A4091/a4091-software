#include "version.h"

#define STR(s) #s      // Turn s into a string literal without macro expansion
#define XSTR(s) STR(s) // Turn s into a string literal after macro expansion
const char * const version_str =
    XSTR(DEVICE_VERSION)"."XSTR(DEVICE_REVISION)" built "BUILD_DATE" "BUILD_TIME;