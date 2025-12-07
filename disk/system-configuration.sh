#!/bin/bash

# Write a system-configuration file. 
#
# Check /opt/amiga/m68k-amigaos/ndk-include/intuition/preferences.h for information.

SC=system-configuration
    (
    # the default font height
    printf "\x08"     # FontHeight (80 columns)
    # printf "\x09"     # FontHeight (60 columns)

    # constant describing what's hooked up to the printer port
    printf "\x00"     # PrinterPort
    # Baud rate of the serial port
    printf "\x00\x05" # BaudRate

    # various timing rates
    printf "\x00\x00\x00\x00" # KeyReptSpeed
    printf "\x00\x00\xc3\x50"
    printf "\x00\x00\x00\x00" # KeyRptDelay
    printf "\x00\x09\x27\xc0"
    printf "\x00\x00\x00\x01" # DoubleClick
    printf "\x00\x07\xa1\x20"

    # Intuition Pointer Data
    printf "\x00\x00\x00\x00\xc0\x00\x40\x00" # PointerMatrix
    printf "\x70\x00\xb0\x00\x3c\x00\x4c\x00"
    printf "\x3f\x00\x43\x00\x1f\xc0\x20\xc0"
    printf "\x1f\xc0\x20\x00\x0f\x00\x11\x00"
    printf "\x0d\x80\x12\x80\x04\xc0\x09\x40"
    printf "\x04\x60\x08\xa0\x00\x20\x00\x40"
    printf "\x00\x00\x00\x00\x00\x00\x00\x00"
    printf "\x00\x00\x00\x00\x00\x00\x00\x00"
    printf "\x00\x00\x00\x00\x00\x00\x00\x00"
    printf "\xff"     # XOffset
    printf "\x00"     # YOffset
    printf "\x0e\x44" # color17 (Sprite Colors)
    printf "\x00\x00" # color18
    printf "\x0e\xec" # color19
    printf "\x00\x01" # PointerTicks

    # Workbench Screen Colors
    printf "\x0a\xaa" # color0
    printf "\x00\x00" # color1
    printf "\x0f\xff" # color2
    printf "\x06\x8b" # color3
    #printf "\x0b\x86" # color3

    # positioning data for the intuition view
    printf "\x00"     # ViewXOffset
    printf "\x00"     # ViewYOffset
    printf "\x00\x81" # ViewInitX 
    printf "\x00\x2c" # ViewInitY

    printf "\x00\x00" # EnableCLI

    # printer configuration
    printf "\x00\x00" # PrinterType (Custom)
    printf "\x67\x65\x6e\x65\x72\x69\x63\x00\x00\x00\x00\x00\x00\x00\x00" # PrinterFilename
    printf "\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00"
    printf "\x00\x00" # PrintPitch (Pica)
    printf "\x00\x00" # PrintQuality (Draft)
    printf "\x00\x00" # PrintSpacing (Six LPI)
    printf "\x00\x05" # PrintLeftMargin
    printf "\x00\x4b" # PrintRightMargin
    printf "\x00\x00" # PrintImage
    printf "\x00\x00" # PrintAspect
    printf "\x00\x01" # PrintShade (Greyscale)
    printf "\x00\x02" # PrintThreshold
    printf "\x00\x20" # PaperSize (Tractor)
    printf "\x00\x42" # PaperLength
    printf "\x00\x00" # PaperType

    # Serial Device Settings
    printf "\x00"     # SerRWBits
    printf "\x00"     # SerStopBuf
    printf "\x00"     # SerParShk

    # Workbench interlaced?
    # printf "\x01"     # LaceWB (On)
    printf "\x00"     # LaceWB (Off)

    printf "\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00" # Pad 
    printf "\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00" # PrtDevName

    printf "\x00"     # DefaultPrtUnit
    printf "\x00"     # DefaultSerUnit

    printf "\x00"     # RowSizeChange
    printf "\x00"     # ColumnSizeChange

    printf "\x00\x00" # PrintFlags
    printf "\x00\x00" # PrintMaxWidth
    printf "\x00\x00" # PrintMaxHeight
    printf "\x00"     # PrintDensity
    printf "\x00"     # PrintXOffset

    printf "\x00\x00" # wb_Width
    printf "\x00\x00" # wb_Height
    printf "\x00"     # wb_Depth

    printf "\x00"     # ext_size
) > $SC
