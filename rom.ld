/* rom.ld - vlink linker script for a4091 rom */
SECTIONS {
    /* All the code in rom.S etc. */
    .bootldr : {
        *(CODE)
        *(DATA)
        *(BSS)
    }
    /* Visible at 0x2000 but this lives at 0x800 */
    .device : {
	. = 0x800;
	*(.device)
    }
    /* How is this done the right way? */
    .fill : {
	. = ALIGN(0x8000)-8;
	LONG(0xFFFF5352);
	LONG(0x2F434448);
    } =0xffff
}
