//
// Copyright 2022-2025 Chris Hooper & Stefan Reinauer
//
// Redistribution and use in source and binary forms, with or without
// modification, are permitted provided that the following conditions are met:
//
// 1. Redistributions of source code must retain the above copyright notice,
//    this list of conditions and the following disclaimer.
// 2. Redistributions in binary form must reproduce the above copyright notice,
//    this list of conditions and the following disclaimer in the documentation
//    and/or other materials provided with the distribution.
//

#include "version.h"
#include "port.h"

#define STR(s) #s      // Turn s into a string literal without macro expansion
#define XSTR(s) STR(s) // Turn s into a string literal after macro expansion
#ifdef DEBUG_DEVICE
const char * const version_str =
    XSTR(DEVICE_VERSION)"."XSTR(DEVICE_REVISION)" built "BUILD_DATE" "BUILD_TIME;
#endif

#define DEVICE_ID_STRING XSTR(DEVNAME) " " XSTR(DEVICE_VERSION) "." \
        XSTR(DEVICE_REVISION) " (" AMIGA_DATE ")\r\n"
        /* format: "name version.revision (dd.mm.yyyy)" */
const char device_id_string[] = DEVICE_ID_STRING;
