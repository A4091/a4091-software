/* rom.ld - vlink linker script for the A4091 AutoConfig ROM */

SECTIONS {
    /* AutoConfig ROM code from rom.S and reloc.S gets placed
     * at the very beginning of the output file.
     */
    .bootldr : {
        *(CODE)
        *(DATA)
        *(BSS)
    }
    /* Placing the a4091.device in the ROM file right after
     * the AutoConfig ROM.
     */
    .device : {
	*(DEVICE)
    }
    /* Fill the unused space in the ROM with 0xFF. We use this
     * mechanism also to align the size of our ROM to a multiple
     * of 32KB. Placing some data at the end of the segment tricks
     * vlink into playing along.
     */
    .fill : {
	. = ALIGN(0x8000)-8;
	LONG(0xFFFF5352);
	LONG(0x2F434448);
    } =0xffff
}
