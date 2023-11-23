/* rom.ld - vlink linker script for the A4091 AutoConfig ROM */

SECTIONS {
    /* AutoConfig ROM code from rom.S and reloc.S gets placed
     * at the very beginning of the output file.
     */
    rom = .;
    .bootldr : {
        *(CODE)
        *(DATA)
        *(BSS)
    }

    /* Placing the a4091.device and CDFileSystem in the ROM
     * file right after the AutoConfig ROM.
     */
    .device ALIGN(16): {
        device = .;
	*(DEVICE)
    }

    /* Fill the unused space in the ROM with 0xFF. We use this
     * mechanism also to align the size of our ROM to 64kB.
     * The end of the ROM contains an eight byte magic number
     * and a TOC with offsets to all relocatable binaries stored
     * in the ROM.
     */
    .fill : {
	. = ALIGN(0x10000) - 40;
	LONG(0xFFFFFFFF);   /* Filesystem 2 DosType */
	LONG(0x00000000);   /* Filesystem 2 offset */
	LONG(0x00000000);   /* Filesystem 2 length */
	LONG(0xFFFFFFFF);   /* Filesystem 1 DosType */
	LONG(0x00000000);   /* Filesystem 1 offset */
	LONG(0x00000000);   /* Filesystem 1 length */
	LONG(device - rom); /* Device driver offset */
	LONG(0x00000000);   /* Device driver length */
	LONG(0xFFFF5352);
	LONG(0x2F434448);
    } =0xffff
}
