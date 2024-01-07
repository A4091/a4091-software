AGUIDECHECK=Tools/AGuideCheck/AGuideCheck

disk: scsifix
	./createdisk.sh

clean:
	rm -f a4091_??.??.adf
	rm -f NDK3.2.lha
	rm -rf Tools

check: $(AGUIDECHECK)
	vamos $(AGUIDECHECK) A4091.guide

$(AGUIDECHECK):
	wget http://aminet.net/dev/misc/NDK3.2.lha
	lha xqf NDK3.2.lha Tools/AGuideCheck/* Tools/AG2HTML/*

scsifix.o: scsifix.asm
	vasmm68k_mot -Fhunk -o scsifix.o scsifix.asm
scsifix: scsifix.o
	vlink -bamigahunk -o scsifix -s scsifix.o
clean:
	rm scsifix.o scsifix
