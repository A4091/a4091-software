#!/bin/bash

set -e

if [ -z "${FULL_VERSION:-}" ]; then
  FULL_VERSION=$(git describe --tags --dirty | sed -r 's/^release_//')
fi
if [ -z "${ADATE:-}" ]; then
  ADATE=$(date '+%-d.%-m.%Y')
fi
VER=$FULL_VERSION
DISK=scsi_$VER.adf
THIRDPARTY=../3rdparty
DEVICE_NAME=${DEVNAME:-}
FLASH_DEVICE=0
ALL_DEVICES="a4091 a4092 a4770 scsi710 scsi770"
GUIDE_BASENAMES="A4091 A4092 A4770 A4000T A4000D+ A4000T770"
GUIDE_SOURCE=A4091.guide
GUIDE_INFO_SOURCE=A4091.guide.info
TOOLS_INFO_SOURCE=Tools.info
A4092FLASH_INFO_SOURCE=a4092flash.info
GUIDE_TMP=A4091.guide.tmp
STARTUP_SEQUENCE_TMP=Startup-Sequence.tmp
UPDATE_SCRIPT_TMP=update.tmp

cleanup() {
  rm -f "$STARTUP_SEQUENCE_TMP" "$UPDATE_SCRIPT_TMP" "$GUIDE_TMP"
}
trap cleanup EXIT

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

guide_base_from_dev() {
  case "$1" in
    a4091) echo "A4091" ;;
    a4092) echo "A4092" ;;
    a4770) echo "A4770" ;;
    scsi710) echo "A4000T" ;;
    scsi770) echo "A4000D+" ;;
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

delete_guide_files() {
  local disk=$1 guide_base
  for guide_base in $GUIDE_BASENAMES; do
    delete_if_present "$disk" "${guide_base}.guide"
    delete_if_present "$disk" "${guide_base}.guide.info"
  done
}

render_guide_file() {
  local dev=$1
  sed -e "s|@A4091_DISK_FULL_VERSION@|${VER}|g" \
      -e "s|@A4091_DISK_BUILD_DATE@|${ADATE}|g" \
      -e "s|@A4091_DISK_DEVICE_NAME@|${dev}.device|g" \
      < "$GUIDE_SOURCE" > "$GUIDE_TMP"
}

write_guide_files() {
  local disk=$1 dev=$2 guide_base
  if ! guide_base=$(guide_base_from_dev "$dev"); then
    echo "No guide filename configured for device '$dev'."
    exit 1
  fi
  render_guide_file "$dev"
  xdftool "$disk" write "$GUIDE_TMP" "${guide_base}.guide"
  xdftool "$disk" write "$GUIDE_INFO_SOURCE" "${guide_base}.guide.info"
}

delete_flash_tool_icons() {
  local disk=$1
  delete_if_present "$disk" "Tools.info"
  delete_if_present "$disk" "Tools/a4092flash.info"
}

write_flash_tool_icons() {
  local disk=$1 dev=$2
  if is_flash_device "$dev"; then
    xdftool "$disk" write "$TOOLS_INFO_SOURCE" "Tools.info"
    xdftool "$disk" write "$A4092FLASH_INFO_SOURCE" "Tools/a4092flash.info"
  fi
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

sed -e "s|@A4091_DISK_VERSION_LINE@|$VERSION_LINE|" \
    -e "s|@A4091_DISK_COPYRIGHT_LINE@|$COPYRIGHT_LINE|" \
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
write_guide_files "$DISK" "$DEVICE_NAME"
write_flash_tool_icons "$DISK" "$DEVICE_NAME"
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

  delete_guide_files "$VDISK"
  write_guide_files "$VDISK" "$dev"

  delete_flash_tool_icons "$VDISK"
  write_flash_tool_icons "$VDISK" "$dev"

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

echo "Cleaning up..."
cleanup
trap - EXIT

exit 0
