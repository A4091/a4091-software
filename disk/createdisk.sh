#!/bin/bash

VER=$(git describe --tags --dirty | sed -r 's/^release_//')
DISK=scsi_$VER.adf
THIRDPARTY=../3rdparty

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
xdftool $DISK write ../ncr7xx Tools/ncr7xx
xdftool $DISK write ../a4091d Tools/a4091d
xdftool $DISK write scsifix Tools/scsifix
xdftool $DISK write ../util/a4092flash/a4092flash Tools/a4092flash
xdftool $DISK write $THIRDPARTY/devtest/devtest Tools/devtest
xdftool $DISK write $THIRDPARTY/RDBFlags/RDBFlags Tools/RDBFlags
xdftool $DISK makedir Devs
xdftool $DISK write system-configuration Devs/system-configuration
for dev in a4091 a4092 scsi710 scsi770; do
  [ -r ../${dev}.device ] && xdftool $DISK write ../${dev}.device Devs/${dev}.device
done
if [ -r ../a4092_cdfs.rom ]; then
xdftool $DISK makedir ROMs
xdftool $DISK write ../a4092_cdfs.rom ROMs/a4092_cdfs.rom
fi
xdftool $DISK write update_a4092 Tools/update
xdftool $DISK protect Tools/update sparwed
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
rm Startup-Sequence.tmp

echo "Done. Please verify disk contents of $DISK below:"
echo "------------------------------------------------------------------------------------------"
xdftool $DISK list
echo "------------------------------------------------------------------------------------------"
echo "Created $DISK"
ALL_DEVICES="a4091 a4092 ncr710 ncr770"

make_variant() {
  local dev=$1 name=$2 label=$3
  [ -r ../${dev}.device ] || return
  local VDISK=${DISK/scsi/$name}
  cp $DISK $VDISK
  xdftool $VDISK relabel "$label"
  for other in $ALL_DEVICES; do
    [ "$other" != "$dev" ] && [ -r ../${other}.device ] && \
      xdftool $VDISK delete Devs/${other}.device
  done
}

make_variant a4091 a4091 "A4091 SCSI"
make_variant a4092 a4092 "A4092 SCSI"
make_variant ncr710 a4000t "A4000T SCSI"
make_variant ncr770 saft4k "SaftA4K SCSI"

exit 0
