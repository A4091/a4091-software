AGUIDECHECK=Tools/AGuideCheck/AGuideCheck
THIRDPARTY=../3rdparty
DEVTEST=$(THIRDPARTY)/devtest/devtest

disk: scsifix $(DEVTEST) rdb
	./createdisk.sh

$(DEVTEST): FORCE
	$(MAKE) -s -C $(THIRDPARTY)/devtest
	m68k-amigaos-strip -s $@

rdb: $(THIRDPARTY)/bffs/dist/bffs16_src.lha
	lha xiq2f $< bffs_1.6/bin/rdb

clean:
	rm -f *_??.??.adf
	rm -f NDK3.2.lha
	rm -rf Tools
	rm -f scsifix.o scsifix rdb
	$(MAKE) -C $(THIRDPARTY)/devtest clean

FORCE:
.PHONY: clean disk FORCE

check: $(AGUIDECHECK)
	vamos $(AGUIDECHECK) A4091.guide

$(AGUIDECHECK):
	wget http://aminet.net/dev/misc/NDK3.2.lha
	lha xqf NDK3.2.lha Tools/AGuideCheck/* Tools/AG2HTML/*

scsifix.o: scsifix.asm
	vasmm68k_mot -Fhunk -o scsifix.o scsifix.asm
scsifix: scsifix.o
	vlink -bamigahunk -o scsifix -s scsifix.o
