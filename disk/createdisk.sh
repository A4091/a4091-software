

VER=$(awk '/#define DEVICE_/{if (V != "") print V""$NF; else V=$NF}' ../version.h)
DISK=A4091_$VER.adf

echo "Downloading..."
wget -q http://aminet.net/disk/misc/bffs16_src.lha
lha xq bffs16_src.lha
git clone -q https://github.com/cdhooper/amiga_devtest.git

echo "Building..."
make -s -C amiga_devtest || exit 1

echo "Creating disk..."
xdftool $DISK format "Amiga4091"
xdftool $DISK makedir Tools
xdftool $DISK write bffs_1.6/bin/rdb Tools/rdb
xdftool $DISK write ../a4091 Tools/a4091
xdftool $DISK write ../a4091d Tools/a4091d
xdftool $DISK write amiga_devtest/devtest Tools/devtest
xdftool $DISK write A4091.guide
xdftool $DISK write A4091.guide.info
xdftool $DISK write Disk.info

echo "Cleaning up..."
rm -rf bffs_1.6 bffs16_src.lha
rm -rf amiga_devtest

echo "Done:"
xdftool $DISK list
echo "Created $DISK"
