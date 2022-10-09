

DISK=A4091_4223.adf

wget http://aminet.net/disk/misc/bffs16_src.lha
lha x bffs16_src.lha
git clone https://github.com/cdhooper/amiga_devtest.git
cd amiga_devtest
make
cd ..


xdftool $DISK format "Amiga4091"
xdftool $DISK makedir Tools
xdftool $DISK write bffs_1.6/bin/rdb Tools/rdb
xdftool $DISK write ../a4091 Tools/a4091
xdftool $DISK write ../a4091d Tools/a4091d
xdftool $DISK write amiga_devtest/devtest Tools/devtest
xdftool $DISK write A4091.guide
xdftool $DISK write A4091.guide.info
xdftool $DISK write Disk.info

rm -rf bffs_1.6 bffs16_src.lha
rm -rf amiga_devtest

