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

    /* Placing the a4091.device in the ROM file right after
     * the AutoConfig ROM.
     */
    .device ALIGN(16): {
        device = .;
	*(DEVICE)
    }

    /* Add CDFileSystem for CDROM boot. Requires 64K ROM */
    .filesystem ALIGN(16): {
        filesystem = .;
	*(FILESYSTEM)
    }

    /* Fill the unused space in the ROM with 0xFF. We use this
     * mechanism also to align the size of our ROM to a multiple
     * of 32KB.
     * The end of the ROM contains an eight byte magic number
     * and a TOC with offsets to all relocatable binaries stored
     * in the ROM.
     */
    .fill : {
	. = ALIGN(0x8000) - 16;
	LONG(filesystem - rom);
	LONG(device - rom);
	LONG(0xFFFF5352);
	LONG(0x2F434448);
    } =0xffff
}
