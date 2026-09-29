PROGS := smolalsa_test smolalsa_play smolalsa_rec smolalsa_ctl smolalsa_mod

HEADERS += smolalsa.h

# Both builds come out of smolcommon.mk. SMOL_ARCH picks a target and it will
# say what else that wants: NOLIBCDIR, NOLIBCEXTDIR and CROSS_COMPILE. Upstream
# nolibc cannot self relocate yet, so pass NOPIE=1 with those.
include smolcommon.mk

LIBC_ELFS := $(addsuffix .libc.elf,$(PROGS))

all: $(LIBC_ELFS)

ifdef SMOL_ARCH
ELFS := $(addsuffix .$(SMOL_SUFFIX).elf,$(PROGS))

all: $(ELFS)
endif

.PHONY: all clean
clean:
	$(Q)rm -f .buildmode.* *.elf *.elf.dbg *.tar
