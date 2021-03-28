   MODULE = xot

     DEST = /maint/module/$(MODULE)

INSTALLED = $(foreach f, Driver.o $(ALL), $(DEST)/$f)

      ALL = Master Node Space.c System xot.h

       CC = cc

   LDLIBS = -lsocket

   CFLAGS = -I. -O -D_KERNEL=1 -DXOT_DEBUG

    PARTS = xot.o

    XOT_H = /usr/include/atlantic/xot.h

  VERSION = ../makeversion

all:		xotlink Driver.o

install:	$(INSTALLED) $(DEST)/Makefile

$(PARTS):	xot.h

Driver.o:	$(PARTS)
	$(LD) -r $(PARTS) -o $@
	chmod a+r Driver.o

xot.o:		xot.c xot.h private.h version.h

version.h:	xot.c xot.h private.h
	-$(VERSION) version.h "X25/TCP (XOT) Driver" $^

$(INSTALLED): 	$(DEST)/% : %
	@if [ ! -d $(@D) ]; then mkdir $(@D) ; fi
	cp -f $< $@

$(DEST)/Makefile:	Makefile
	@rm -f zzmakefile
	@echo "install: $(XOT_H)" 				>> zzmakefile
	@echo "	/etc/conf/bin/idinstall -k -M $(MODULE)" 	>> zzmakefile
	@echo "	/etc/conf/bin/idinstall -k -u $(MODULE)" 	>> zzmakefile
	@echo "	/etc/conf/bin/idbuild -M $(MODULE)" 		>> zzmakefile
	@echo 							>> zzmakefile
	@echo "$(XOT_H): xot.h"					>> zzmakefile
	@echo "	@if [ ! -d $$(@D) ]; then mkdir $$(@D); fi"	>> zzmakefile
	@echo "	cp -f $$(@F) $$(@D)"				>> zzmakefile
	@echo 							>> zzmakefile
	cp -f zzmakefile $@
	@rm zzmakefile

xot.tgz: LICENSE README $(ALL) Makefile version.h xot.c private.h xotlink.c xotlink.cf
	tar czvf $@ $^
