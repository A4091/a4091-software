#include "version.h"
#include "port.h"

#define STR(s) #s      // Turn s into a string literal without macro expansion
#define XSTR(s) STR(s) // Turn s into a string literal after macro expansion
#ifdef DEBUG_DEVICE
const char * const version_str =
    XSTR(DEVICE_VERSION)"."XSTR(DEVICE_REVISION)" built "BUILD_DATE" "BUILD_TIME;
#endif

#define DEVICE_ID_STRING "a4091 " XSTR(DEVICE_VERSION) "." \
        XSTR(DEVICE_REVISION) " (" AMIGA_DATE ")\r\n"
        /* format: "name version.revision (dd.mm.yyyy)" */
const char device_id_string[] = DEVICE_ID_STRING;
