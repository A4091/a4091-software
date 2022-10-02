#ifdef DEBUG_DEVICE
#define USE_SERIAL_OUTPUT
#endif
#include "port.h"
#include "printf.h"
#include <stdlib.h>
#include <stdio.h>
#include <resources/battmem.h>
#include <pragmas/exec_pragmas.h>
#include <clib/battmem_protos.h>
#include <pragmas/battmem_pragmas.h>

#include "device.h"
#include "attach.h"
#include "battmem.h"

struct Library *BattMemBase;

int Load_BattMem(void)
{
    UBYTE cdrom_boot = 0;

    BattMemBase = OpenResource(BATTMEMNAME);
    if (!BattMemBase)
        return 0;

    ObtainBattSemaphore();
    printf("Retrieving settings from BattMem\n");
    ReadBattMem(&cdrom_boot,
                BATTMEM_A4091_CDROM_BOOT_ADDR,
                BATTMEM_A4091_CDROM_BOOT_LEN);
    // CDROM_BOOT defaults to on, hence invert it
    asave->cdrom_boot = !cdrom_boot;
    printf("  cdrom_boot: %d\n", asave->cdrom_boot);
    ReleaseBattSemaphore();

    return 1;
}

int Save_BattMem(void)
{
    UBYTE cdrom_boot = !asave->cdrom_boot;

    if (!BattMemBase)
        return 0;

    ObtainBattSemaphore();
    printf("Storing settings to BattMem\n");
    printf("  cdrom_boot: %d (%d)\n", asave->cdrom_boot, cdrom_boot);
    WriteBattMem(&cdrom_boot,
                 BATTMEM_A4091_CDROM_BOOT_ADDR,
                 BATTMEM_A4091_CDROM_BOOT_LEN);
    ReleaseBattSemaphore();

    return 1;
}



