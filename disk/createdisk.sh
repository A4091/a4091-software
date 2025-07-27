

VER=$(awk '/#define DEVICE_/{if (V != "") print V""$NF; else V=$NF"."}' ../version.h)
DISK=a4091_$VER.adf
THIRDPARTY=../3rdparty

echo "Looking for submodules..."
test ! -x $THIRDPARTY/bffs/dist/bffs16_src.lha || ( echo "Initialize submodules."; exit 1 )

echo "Building devtest..."
make -s -C $THIRDPARTY/devtest || exit 1
echo "Extracting rdb..."
lha xiq2f $THIRDPARTY/bffs/dist/bffs16_src.lha bffs_1.6/bin/rdb

sed s/VERSION/${VER}/ < Startup-Sequence > Startup-Sequence.tmp

echo "Creating disk..."
xdftool $DISK format "Amiga4091"
xdftool $DISK makedir Tools
xdftool $DISK write rdb Tools/rdb
xdftool $DISK write ../a4091 Tools/a4091
xdftool $DISK write ../a4091d Tools/a4091d
xdftool $DISK write scsifix Tools/scsifix
xdftool $DISK write $THIRDPARTY/devtest/devtest Tools/devtest
xdftool $DISK write $THIRDPARTY/RDBFlags/RDBFlags Tools/RDBFlags
xdftool $DISK makedir Devs
xdftool $DISK write ../a4091.device Devs/a4091.device
xdftool $DISK write A4091.guide
xdftool $DISK write A4091.guide.info
xdftool $DISK write Disk.info
xdftool $DISK makedir S
xdftool $DISK write Startup-Sequence.tmp S/Startup-Sequence
xdftool $DISK boot install

echo "Cleaning up..."
make -s -C $THIRDPARTY/devtest clean
rm rdb
rm Startup-Sequence.tmp

echo "Done. Please verify disk contents of $DISK below:"
echo "------------------------------------------------------------------------------------------"
xdftool $DISK list
echo "------------------------------------------------------------------------------------------"
echo "Created $DISK"
