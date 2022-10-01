	XREF ___a4_init

        section .text,code

	public  ___restore_a4
        public  _geta4
___restore_a4:
_geta4:
        lea     ___a4_init,a4
        rts

