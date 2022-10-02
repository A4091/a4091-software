        ; keep out of the Diag area copied to RAM automatically
        section DEVICE
        public  _device
_device
        ifnd NO_DEVICE
        ifd COMMODORE_DEVICE
        incbin "a3090.ld_strip"
        else
        incbin "a4091.device"
        endif
        endif

        section FILESYSTEM
        public  _CDFileSystem
_CDFileSystem
        ifnd NO_DEVICE
        ifd CDFS
        incbin "BootCDFileSystem"
        endif
        endif

        CODE

