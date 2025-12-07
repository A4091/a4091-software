

VER=$(git describe --tags --dirty | sed -r 's/^release_//')
DISK=a4091_$VER.adf
THIRDPARTY=../3rdparty

echo "Looking for submodules..."
test ! -x $THIRDPARTY/bffs/dist/bffs16_src.lha || ( echo "Initialize submodules."; exit 1 )

echo "Building devtest..."
make -s -C $THIRDPARTY/devtest || exit 1
m68k-amigaos-strip -s $THIRDPARTY/devtest/devtest

echo "Extracting rdb..."
lha xiq2f $THIRDPARTY/bffs/dist/bffs16_src.lha bffs_1.6/bin/rdb

# Create centered version and copyright lines (80 column terminal)
COLUMNS=80
YEAR=$(date +%Y)
VERSION_TEXT="v${VER}"
COPYRIGHT_TEXT='\xa9'" 2023-${YEAR} Stefan Reinauer \\& Chris Hooper"
VERSION_SPACES=$(( (${COLUMNS} - ${#VERSION_TEXT}) / 2 ))
COPYRIGHT_SPACES=$(( (${COLUMNS} - ${#COPYRIGHT_TEXT}) / 2 ))
VERSION_LINE=$(printf "%*s%s" $VERSION_SPACES "" "$VERSION_TEXT")
COPYRIGHT_LINE=$(printf "%*s%s" $COPYRIGHT_SPACES "" "$COPYRIGHT_TEXT")

sed -e "s|VERSION_LINE|$VERSION_LINE|" \
    -e "s|COPYRIGHT_LINE|$COPYRIGHT_LINE|" \
    < Startup-Sequence > Startup-Sequence.tmp

./system-configuration.sh
echo "Creating disk..."
xdftool $DISK format "Amiga4091"
xdftool $DISK makedir Tools
xdftool $DISK write rdb Tools/rdb
xdftool $DISK write ../a4091 Tools/a4091
xdftool $DISK write ../a4091d Tools/a4091d
xdftool $DISK write scsifix Tools/scsifix
xdftool $DISK write ../util/a4092flash/a4092flash Tools/a4092flash
xdftool $DISK write $THIRDPARTY/devtest/devtest Tools/devtest
xdftool $DISK write $THIRDPARTY/RDBFlags/RDBFlags Tools/RDBFlags
xdftool $DISK makedir Devs
xdftool $DISK write system-configuration Devs/system-configuration
if [ -r ../a4091.device ]; then
xdftool $DISK write ../a4091.device Devs/a4091.device
fi
if [ -r ../a4092.device ]; then
xdftool $DISK write ../a4092.device Devs/a4092.device
fi
if [ -r ../scsi710.device ]; then
xdftool $DISK write ../scsi710.device Devs/scsi710.device
fi
if [ -r ../scsi770.device ]; then
xdftool $DISK write ../scsi770.device Devs/scsi770.device
fi
if [ -r ../a4092_cdfs.rom ]; then
xdftool $DISK makedir ROMs
xdftool $DISK write ../a4092_cdfs.rom ROMs/a4092_cdfs.rom
fi
xdftool $DISK write A4091.guide
xdftool $DISK write A4091.guide.info
xdftool $DISK write Disk.info
xdftool $DISK makedir S
xdftool $DISK write Startup-Sequence.tmp S/Startup-Sequence
xdftool $DISK boot install

printf "Looking for additional files... "
if [ -x ../../a4091-disk-addon/add-to-disk.sh ]; then
  echo "found."
  ../../a4091-disk-addon/add-to-disk.sh "$DISK"
else
  echo "not found."
fi

echo "Cleaning up..."
make -s -C $THIRDPARTY/devtest clean
rm rdb
rm Startup-Sequence.tmp

echo "Done. Please verify disk contents of $DISK below:"
echo "------------------------------------------------------------------------------------------"
xdftool $DISK list
echo "------------------------------------------------------------------------------------------"
echo "Created $DISK"
