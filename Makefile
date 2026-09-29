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
TARBALL := smolalsa.$(SMOL_SUFFIX).tar

all: $(ELFS)

# A tarball of them for dropping into a rootfs, laid out by rootfs.tarwak.json.
# Pass TARWAK=<path to the tarwak binary>, and TARWAK_FEATURES=devnodes to pack
# /dev/snd as well, since the machine this is aimed at has no udev.
ifdef TARWAK
all: $(TARBALL)

$(TARBALL): rootfs.tarwak.json $(ELFS)
	$(MSG) TARWAK $@
	$(Q)$(TARWAK) -i $< -o $@ -b ./ -p "%s.$(SMOL_SUFFIX).elf" \
		$(addprefix -f ,$(TARWAK_FEATURES))
endif
endif

.PHONY: all clean
clean:
	$(Q)rm -f .buildmode.* *.elf *.elf.dbg *.tar
