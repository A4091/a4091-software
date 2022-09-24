        ; keep out of the Diag area copied to RAM automatically
        section DEVICE
        public  _device
_device
        ifd COMMODORE_DEVICE
        incbin "a3090.ld_strip"
        else
        incbin "a4091.device"
        endif

        section FILESYSTEM
        public  _CDFileSystem
_CDFileSystem
        ifd CDFS
        incbin "BootCDFileSystem"
        endif

        CODE

