#!/bin/bash

set -e

VER=$(git describe --tags --dirty | sed -r 's/^release_//')
DISK=scsi_$VER.adf
THIRDPARTY=../3rdparty
DEVICE_NAME=${DEVNAME:-}
FLASH_DEVICE=0
ALL_DEVICES="a4091 a4092 a4770 scsi710 scsi770"
STARTUP_SEQUENCE_TMP=Startup-Sequence.tmp
UPDATE_SCRIPT_TMP=update.tmp

devname_from_device() {
  case "$1" in
    A4091) echo "a4091" ;;
    A4092) echo "a4092" ;;
    A4770) echo "a4770" ;;
    A4000T) echo "scsi710" ;;
    A4000T770) echo "scsi770" ;;
    *) return 1 ;;
  esac
}

is_flash_device() {
  case "$1" in
    a4092|a4770) return 0 ;;
  esac
  return 1
}

delete_if_present() {
  local disk=$1 path=$2
  xdftool "$disk" delete "$path" >/dev/null 2>&1 || true
}

write_update_script() {
  local rom_name=$1
  sed -e "s|@ROMFILE@|${rom_name}|g" < update > "$UPDATE_SCRIPT_TMP"
}

if [ -z "$DEVICE_NAME" ]; then
  DEVICE_NAME=$(devname_from_device "${DEVICE:-A4091}")
fi

if is_flash_device "$DEVICE_NAME"; then
  FLASH_DEVICE=1
fi

if [ ! -r "../${DEVICE_NAME}.device" ]; then
  echo "Missing ../${DEVICE_NAME}.device. Build the selected target before creating the disk."
  exit 1
fi

if [ "$FLASH_DEVICE" -eq 1 ] && [ ! -r "../${DEVICE_NAME}_cdfs.rom" ]; then
  echo "Missing ../${DEVICE_NAME}_cdfs.rom. Build the selected flash ROM before creating the disk."
  exit 1
fi

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
    < Startup-Sequence > "$STARTUP_SEQUENCE_TMP"

./system-configuration.sh
echo "Creating disk..."
xdftool $DISK format "Amiga4091"
xdftool $DISK makedir Devs
xdftool $DISK makedir ROMs
xdftool $DISK makedir S
xdftool $DISK makedir Tools
xdftool $DISK write rdb Tools/rdb
xdftool $DISK write ../ncr7xx Tools/ncr7xx
xdftool $DISK write ../a4091d Tools/a4091d
xdftool $DISK write scsifix Tools/scsifix
xdftool $DISK write ../util/a4092flash/a4092flash Tools/a4092flash
xdftool $DISK write ../util/a4092flash/mfgtool Tools/mfgtool
xdftool $DISK write ../util/a4092flash/mfg.cfg S/mfg.txt
xdftool $DISK write ../util/a4092flash/serial.txt S/serial.txt
xdftool $DISK write $THIRDPARTY/devtest/devtest Tools/devtest
xdftool $DISK write $THIRDPARTY/RDBFlags/RDBFlags Tools/RDBFlags
xdftool $DISK write system-configuration Devs/system-configuration
xdftool $DISK write ../${DEVICE_NAME}.device Devs/${DEVICE_NAME}.device
if [ "$FLASH_DEVICE" -eq 1 ]; then
  ROM_NAME=${DEVICE_NAME}_cdfs.rom
  xdftool $DISK write ../${ROM_NAME} ROMs/${ROM_NAME}
  write_update_script "$ROM_NAME"
  xdftool $DISK write "$UPDATE_SCRIPT_TMP" Tools/update
  xdftool $DISK protect Tools/update sparwed
fi
xdftool $DISK write A4091.guide
xdftool $DISK write A4091.guide.info
xdftool $DISK write Disk.info
xdftool $DISK write "$STARTUP_SEQUENCE_TMP" S/Startup-Sequence
xdftool $DISK boot install

printf "Looking for additional files... "
if [ -x ../../a4091-disk-addon/add-to-disk.sh ]; then
  echo "found."
  ../../a4091-disk-addon/add-to-disk.sh "$DISK"
else
  echo "not found."
fi

echo "Cleaning up..."
rm -f "$STARTUP_SEQUENCE_TMP" "$UPDATE_SCRIPT_TMP"

echo "Done. Please verify disk contents of $DISK below:"
echo "------------------------------------------------------------------------------------------"
xdftool $DISK list
echo "------------------------------------------------------------------------------------------"
echo "Created $DISK"

make_variant() {
  local dev=$1 name=$2 label=$3
  [ -r ../${dev}.device ] || return 0
  if is_flash_device "$dev" && [ ! -r ../${dev}_cdfs.rom ]; then
    return 0
  fi
  local VDISK=${DISK/scsi/$name}
  cp $DISK $VDISK
  xdftool $VDISK relabel "$label"

  for other in $ALL_DEVICES; do
    [ "$other" != "$dev" ] && delete_if_present "$VDISK" "Devs/${other}.device"
  done

  delete_if_present "$VDISK" "Devs/${dev}.device"
  xdftool $VDISK write ../${dev}.device Devs/${dev}.device

  delete_if_present "$VDISK" "ROMs/a4092_cdfs.rom"
  delete_if_present "$VDISK" "ROMs/a4770_cdfs.rom"

  if is_flash_device "$dev"; then
    local rom_name=${dev}_cdfs.rom
    xdftool $VDISK write ../${rom_name} ROMs/${rom_name}
    delete_if_present "$VDISK" "Tools/update"
    write_update_script "$rom_name"
    xdftool $VDISK write "$UPDATE_SCRIPT_TMP" Tools/update
    xdftool $VDISK protect Tools/update sparwed
  else
    delete_if_present "$VDISK" "Tools/update"
  fi
}

make_variant a4091 a4091 "A4091 SCSI"
make_variant a4092 a4092 "A4092 SCSI"
make_variant a4770 a4770 "A4770 SCSI"
make_variant scsi710 a4000t "A4000T SCSI"
make_variant scsi770 saft4k "A4000D+ SCSI"

exit 0
