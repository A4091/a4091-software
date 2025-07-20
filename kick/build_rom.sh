#!/bin/sh
#
# Usage:  build_rom.sh <device_name>          # e.g. build_rom.sh scsi770.device

### 0. OS detection ###########################################################
case "$(uname)" in
  Darwin)
    CAPITOLINE=capcli.MacOS ;;
  Linux)
    CAPITOLINE=capcli.Linux ;;
  *)
    echo "❌  Unsupported OS: $(uname). Only macOS and Linux are supported."
    exit 1
    ;;
esac

### 1. Vars & sanity checks ###################################################
SCSIDEV=$1
[ -z "${SCSIDEV}" ] && { echo "Usage: $0 <scsi_device>.device"; exit 1; }

CURRWD=$PWD
KICKDIR="${CURRWD}/kick"
OS_NAME="AmigaOS-3.2.3.lha"
KICK_LHA="${KICKDIR}/${OS_NAME}"
ROM_NAME="A4000T.47.115.rom"
CAP_NAME="Capitoline.Release.2025.04.05.zip"
CAP_ZIP_URL="http://capitoline.twocatsblack.com/wp-content/uploads/2025/04/${CAP_NAME}"
CAP_ZIP_FILE="${KICKDIR}/${CAP_NAME}"
CAP_DIR="${KICKDIR}/Capitoline"
ROM_PATH="${CAP_DIR}/ROMs/${ROM_NAME}"
ADF_NAME="ModulesA4000T_3.2.3.adf"
ADF_PATH="${CAP_DIR}/ADFs/${ADF_NAME}"


if [ ! -f "${KICK_LHA}" ]; then
  echo "❌ No ${OS_NAME} found in kick/."
  exit 1;
fi

### 2. Download Capitoline if not already present #############################
if [ ! -d "${CAP_DIR}" ]; then
  echo "→ Capitoline not found in kick/."
  if [ ! -f "${CAP_ZIP_FILE}" ]; then
    echo "→ Downloading..."
    curl -L "${CAP_ZIP_URL}" -o "${CAP_ZIP_FILE}" || {
      echo "❌ Failed to download Capitoline."
      exit 1
    }
  fi
  echo "→ Unpacking..."
  cd ${KICKDIR}
  unzip -q "${CAP_ZIP_FILE}"
  cd -
  echo "✓   Capitoline downloaded and unpacked."
  mkdir -p "${CAP_DIR}/ROMs" "${CAP_DIR}/ADFs"
fi

### 3A. Extract ADF if missing ################################################
if [ ! -f "${ADF_PATH}" ]; then
  echo "→ ADF file missing. Extracting from ${OS_NAME}…"
  (
    cd "${KICKDIR}"
    lha xi ${OS_NAME} Update3.2.3/ADFs/${ADF_NAME}
    mv "${KICKDIR}/${ADF_NAME}" "${CAP_DIR}/ADFs/"
  )
  echo "✓   ADF extraction complete."
else
  echo "✓   ADF found."
fi

### 3B. Extract ROM if missing ################################################
if [ ! -f "${ROM_PATH}" ]; then
  echo "→ ROM file missing. Extracting from ${OS_NAME}…"
  (
    cd "${KICKDIR}"
    lha xi ${OS_NAME} Update3.2.3/ROMs/${ROM_NAME}
    mv "${ROM_NAME}" "${CAP_DIR}/ROMs/"
  )
  echo "✓   ROM extraction complete."
else
  echo "✓   ROM found."
fi

### 4. Build the ROM ##########################################################
(
  cd "${CAP_DIR}"

  cp "${CURRWD}/${SCSIDEV%.device}.kick" "${SCSIDEV}"
  sed "s/DEVICE/${SCSIDEV}/" "${KICKDIR}/capitoline.script" > capitoline.script

  echo "→ Running Capitoline (${CAPITOLINE})…"
  ./"${CAPITOLINE}" < capitoline.script > "${CURRWD}/capitoline.log"

  cat ROMs/A4000T.E0 ROMs/A4000T.F8 > \
    "${CURRWD}/A4000T_${SCSIDEV%.device}.rom"

  rm -f ROMs/A4000T.E0 ROMs/A4000T.F8 "${SCSIDEV}"
)

echo "✓   ROM created: ${CURRWD}/A4000T_${SCSIDEV%.device}.rom"
