#
# Makefile for the BMMacintosh kernel images.
#
# BMMacintosh is two kernels for each board, one for each Mac: EMU=ii is
# the Mac II and Quadra (Basilisk II, the Pi 4 and 5), EMU=plus the Mac
# Plus (umac, every Pi). Their names are Circle's for the board with
# "kernel" made "macii" or "macplus": macplus.img (Pi Zero), macplus7.img
# (Pi 2), macplus8-32.img (Pi 3), macplus8-rpi4.img and macii8-rpi4.img
# (Pi 4, 64-bit), macplus_2712.img and macii_2712.img (Pi 5, 64-bit). The
# menu switches between them (src/port/machine.c).
#
# Run ./make_all.sh once first; it builds circle-stdlib and then this.
# After that "make" rebuilds the kernels for the board circle-stdlib is
# built for, "make EMU=plus" or "make EMU=ii" one of them.
# ./build_sdcard.sh stages everything for the SD card in build/sdcard.
#

ROOT := $(CURDIR)

ifeq ($(wildcard third_party/circle-stdlib/Config.mk),)
$(error circle-stdlib is not configured. Run ./make_all.sh first)
endif

include third_party/circle-stdlib/Config.mk
# RASPPI (the board) is in Circle's own Config.mk, which circle-stdlib's
# configure writes and Circle's Rules.mk reads; it's needed here first to
# pick the board's display and sound code.
include $(CIRCLEHOME)/Config.mk

# The kernels this board has.
ifneq ($(filter 4 5,$(RASPPI)),)
EMUS := plus ii
else
EMUS := plus
endif

ifeq ($(EMU),)

# Both kernels, one after the other (they share the objects outside
# src/port/ii and src/port/plus).
all:
	@for emu in $(EMUS); do $(MAKE) --no-print-directory EMU=$$emu || exit 1; done

clean:
	@for emu in $(EMUS); do $(MAKE) --no-print-directory EMU=$$emu clean || exit 1; done

distclean: clean
	rm -rf build

.PHONY: all clean distclean

else

ifeq ($(filter $(EMU),$(EMUS)),)
$(error EMU=$(EMU): this board's kernels are $(EMUS))
endif

# The kernel's name: Circle's for the board (Rules.mk), "kernel" made
# "macii" or "macplus".
ifeq ($(RASPPI),1)
TARGET := mac$(EMU)
else ifeq ($(RASPPI),2)
TARGET := mac$(EMU)7
else ifeq ($(RASPPI),3)
TARGET := mac$(EMU)8-32
else ifeq ($(RASPPI),4)
TARGET := mac$(EMU)8-rpi4
else
TARGET := mac$(EMU)_2712
endif

BMMAC_VERSION ?= $(shell tools/version.sh --semver)
BMMAC_GIT ?= $(shell tools/version.sh --git)
ZLIB := third_party/zlib

BUILD := build
GEN := $(BUILD)/gen

OBJS = src/main.o src/kernel.o src/logging.o src/options.o \
       src/port/frontend.o src/port/keymap.o src/port/menu.o src/port/machine.o \
       src/port/osd.o src/port/roms.o src/port/posix_extra.o src/port/fswrap.o \
       src/network.o src/network_time_sync.o \
       src/webui/webui.o src/webui/webui_http.o src/webui/webui_fs.o \
       src/webui/webui_machine.o src/webui/webui_assets.o \
       src/sdcard/sd_fs_fatfs.o src/sdcard/sd_fs_file.o

# zlib's inflate, for MAME's zipped ROM sets (src/port/roms.c).
ZLIB_SRCS = adler32.c crc32.c inffast.c inflate.c inftrees.c zutil.c
ZLIB_CFLAGS = -DHAVE_UNISTD_H -DHAVE_STDARG_H -w
ZLIB_OBJS := $(addprefix $(BUILD)/zlib/,$(ZLIB_SRCS:.c=.o))

ifeq ($(EMU),ii)
include src/port/ii/basilisk.mk
EMU_OBJS := $(addprefix $(BUILD)/b2/,$(B2_CORE_SRCS:.cpp=.o)) \
            $(addprefix $(GEN)/b2/,$(B2_GEN_SRCS:.cpp=.o)) \
            $(foreach n,$(B2_CPUEMU_PARTS),$(GEN)/b2/cpuemu$(n).o) \
            $(addprefix $(BUILD)/slirp/,$(B2_SLIRP_SRCS:.c=.o)) \
            $(addprefix $(BUILD)/slirp/,$(B2_SLIRP_PORT_SRCS:.c=.o))
OBJS += $(addprefix src/port/ii/,$(B2_PORT_SRCS:.cpp=.o)) src/slirp_sockets.o
else
include src/port/plus/umac.mk
EMU_OBJS := $(addprefix $(BUILD)/umac/,$(UMAC_CORE_SRCS:.c=.o)) \
            $(addprefix $(BUILD)/musashi/,$(MUSASHI_SRCS:.c=.o)) \
            $(addprefix $(GEN)/umac/,$(UMAC_GEN_SRCS:.c=.o))
OBJS += $(addprefix src/port/plus/,$(UMAC_PORT_SRCS:.c=.o))
endif
EMU_LIB := $(BUILD)/lib$(EMU).a

# Display and sound. The Pi 2 and 3 use the VideoCore through VCHIQ
# (dispmanx, from BMC64); the Pi 4 and 5 use Circle's framebuffer and HDMI
# sound drivers (src/av/, shared by both; see docs/PI4.md and docs/PI5.md).
ifneq ($(filter 4 5,$(RASPPI)),)
OBJS += src/av/video.o src/av/sound.o src/av/hdmisound.o src/av/scaler.o \
        src/kms/common/filter.o src/kms/common/modes.o \
        src/v3d/gpu.o src/v3d/draw.o src/v3d/crt.o \
        src/v3d/shaders.o
# The direct display driver: the Pi 4's (src/kms/pi4/) or the Pi 5's
# (src/kms/pi5/), with shared filter kernels and mode calculations.
ifeq ($(RASPPI),4)
OBJS += src/kms/pi4/dlist.o src/kms/pi4/hvs.o src/kms/pi4/display.o \
        src/kms/pi4/phy.o src/kms/pi4/modeset.o
else
OBJS += src/kms/pi5/dlist.o src/kms/pi5/hvs.o src/kms/pi5/display.o \
        src/kms/pi5/phy.o src/kms/pi5/modeset.o
endif
# Smooth scaling and the CRT shader are drawn by the ARM: -O3 lets GCC turn
# their per-pixel loops into NEON code (tools/pi4scaler tests the same).
src/av/scaler.o: OPTIMIZE = -O3
else
OBJS += src/fbl.o src/crt_pi_idx.o src/crt_pi_rgb.o src/vchiqsound.o \
        src/vchiqsoundbasedevice.o
endif

include $(CIRCLEHOME)/Rules.mk

INCLUDE := $(CIRCLE_STDLIB_INCLUDES) $(INCLUDE) -I $(CIRCLEHOME)/addon/fatfs

# See src/port/fswrap.c
LDFLAGS += --wrap=stat

VERSION_DEFINES = -DBMMAC_VERSION=\"$(BMMAC_VERSION)\" -DBMMAC_GIT=\"$(BMMAC_GIT)\" \
                  -DBMMAC_RASPPI=$(RASPPI)
CFLAGS += $(VERSION_DEFINES)
CPPFLAGS += $(VERSION_DEFINES)

src/port/roms.o src/port/roms.d: CFLAGS += -I $(ZLIB)

$(BUILD)/zlib/%.o: $(ZLIB)/%.c
	@mkdir -p $(dir $@)
	@echo "  CC    $<"
	@$(CC) $(CFLAGS) $(C_STANDARD) $(ZLIB_CFLAGS) -c -o $@ $<

ifeq ($(EMU),ii)

# Basilisk II, compiled into build/ so the submodule stays clean.
B2_FLAGS = $(CPPFLAGS) $(B2_CXXFLAGS) $(B2_INCLUDES) -I $(GEN)/b2
src/port/ii/%.o src/port/ii/%.d: CPPFLAGS += $(B2_CXXFLAGS) $(B2_INCLUDES) -I $(GEN)/b2
# Disk images through FatFs itself, past newlib's 2 GB lseek (sys_ii.cpp).
src/port/ii/sys_ii.o src/port/ii/sys_ii.d: CPPFLAGS += -DII_SD_FS
$(addprefix src/port/ii/,$(B2_PORT_SRCS:.cpp=.o)): $(GEN)/b2/cputbl.h

# The CPU core's generated sources, as Basilisk II's build makes them
# (Unix/Makefile.in): build68k and gencpu are built for this computer,
# with its compiler.
HOSTCC ?= cc
HOSTCXX ?= c++

$(GEN)/b2/build68k: $(B2)/$(B2_UAE)/build68k.c
	@mkdir -p $(GEN)/b2
	@echo "  HOSTCC $@"
	@$(HOSTCC) $(B2_INCLUDES) -o $@ $<

$(GEN)/b2/cpudefs.cpp: $(GEN)/b2/build68k $(B2)/$(B2_UAE)/table68k
	@echo "  GEN   $@"
	@$(GEN)/b2/build68k < $(B2)/$(B2_UAE)/table68k > $@

$(GEN)/b2/gencpu: $(GEN)/b2/cpudefs.cpp $(B2)/$(B2_UAE)/gencpu.c $(B2)/$(B2_UAE)/readcpu.cpp
	@echo "  HOSTCXX $@"
	@$(HOSTCXX) $(B2_DEFINES) $(B2_INCLUDES) -I $(GEN)/b2 -w -o $@ \
	    -x c++ $(B2)/$(B2_UAE)/gencpu.c $(B2)/$(B2_UAE)/readcpu.cpp $(GEN)/b2/cpudefs.cpp

# gencpu writes into the current folder.
$(GEN)/b2/cpuemu.cpp: $(GEN)/b2/gencpu
	@echo "  GEN   $(GEN)/b2/cpuemu.cpp"
	@cd $(GEN)/b2 && ./gencpu

$(GEN)/b2/cpustbl.cpp $(GEN)/b2/cpufunctbl.cpp $(GEN)/b2/cputbl.h: $(GEN)/b2/cpuemu.cpp

# Static pattern rules: Circle's own %.o: %.cpp (Rules.mk) would otherwise
# be used for these, which are .cpp files next to their .o (make 3.81,
# macOS's, takes the first rule that matches).
B2_CPUEMU_OBJS := $(foreach n,$(B2_CPUEMU_PARTS),$(GEN)/b2/cpuemu$(n).o)
$(B2_CPUEMU_OBJS): $(GEN)/b2/cpuemu%.o: $(GEN)/b2/cpuemu.cpp src/port/ii/sysdeps.h
	@echo "  CPP   $< (part $*)"
	@$(CPP) $(B2_FLAGS) -DPART_$* -c -o $@ $<

B2_GEN_OBJS := $(addprefix $(GEN)/b2/,$(B2_GEN_SRCS:.cpp=.o))
$(B2_GEN_OBJS): $(GEN)/b2/%.o: $(GEN)/b2/%.cpp $(GEN)/b2/cputbl.h src/port/ii/sysdeps.h
	@echo "  CPP   $<"
	@$(CPP) $(B2_FLAGS) -c -o $@ $<

$(BUILD)/b2/extfs.o: B2_FLAGS += $(B2_EXTFS_FLAGS)
$(BUILD)/b2/extfs.o: src/port/ii/extfs_posix.h
$(BUILD)/b2/%.o: $(B2)/%.cpp $(GEN)/b2/cputbl.h src/port/ii/sysdeps.h
	@mkdir -p $(dir $@)
	@echo "  CPP   $<"
	@$(CPP) $(B2_FLAGS) -c -o $@ $<

# slirp, on Circle's sockets (src/slirp_sockets.cpp; circle_sockets.h
# renames its socket calls).
$(BUILD)/slirp/%.o: $(B2)/slirp/%.c src/port/ii/slirp/config.h src/port/ii/slirp/circle_sockets.h \
                    src/port/ii/slirp/circle/sys/uio.h
	@mkdir -p $(dir $@)
	@echo "  CC    $<"
	@$(CC) $(CFLAGS) $(C_STANDARD) $(B2_SLIRP_CFLAGS) -I src/port/ii/slirp/circle \
	    -include src/port/ii/slirp/circle_sockets.h -c -o $@ $<

$(BUILD)/slirp/%.o: src/port/ii/slirp/%.c src/port/ii/slirp/config.h \
                    src/port/ii/slirp/circle_sockets.h
	@mkdir -p $(dir $@)
	@echo "  CC    $<"
	@$(CC) $(CFLAGS) $(C_STANDARD) $(B2_SLIRP_CFLAGS) -I src/port/ii/slirp/circle \
	    -include src/port/ii/slirp/circle_sockets.h -c -o $@ $<

else

# umac and Musashi, compiled into build/ so the submodule stays clean.
UMAC_FLAGS = $(CFLAGS) $(UMAC_CFLAGS) $(UMAC_INCLUDES) -I $(GEN)/umac
src/port/plus/%.o src/port/plus/%.d: CFLAGS += $(UMAC_INCLUDES) -I $(GEN)/umac \
	-include src/port/plus/umac_prefix.h
$(addprefix src/port/plus/,$(UMAC_PORT_SRCS:.c=.o)): $(GEN)/umac/m68kops.h

HOSTCC ?= cc

# Musashi's opcode handlers, written by its m68kmake from m68k_in.c.
$(GEN)/umac/m68kmake: $(MUSASHI)/m68kmake.c
	@mkdir -p $(GEN)/umac
	@echo "  HOSTCC $@"
	@$(HOSTCC) -o $@ $<

$(GEN)/umac/m68kops.h: $(GEN)/umac/m68kops.c

# m68kmake writes m68kops.h with it.
$(GEN)/umac/m68kops.c: $(GEN)/umac/m68kmake $(MUSASHI)/m68k_in.c
	@echo "  GEN   $(GEN)/umac/m68kops.c"
	@$(GEN)/umac/m68kmake $(GEN)/umac $(MUSASHI)/m68k_in.c > /dev/null

UMAC_GEN_OBJS := $(addprefix $(GEN)/umac/,$(UMAC_GEN_SRCS:.c=.o))
$(UMAC_GEN_OBJS): $(GEN)/umac/%.o: $(GEN)/umac/%.c
	@echo "  CC    $<"
	@$(CC) $(UMAC_FLAGS) -c -o $@ $<

$(BUILD)/umac/main.o: UMAC_FLAGS += $(UMAC_MAIN_CFLAGS)

$(BUILD)/umac/%.o: $(UMAC)/src/%.c $(GEN)/umac/m68kops.h src/port/plus/sonydrv.h
	@mkdir -p $(dir $@)
	@echo "  CC    $<"
	@$(CC) $(UMAC_FLAGS) -c -o $@ $<

$(BUILD)/musashi/%.o: $(MUSASHI)/%.c $(GEN)/umac/m68kops.h
	@mkdir -p $(dir $@)
	@echo "  CC    $<"
	@$(CC) $(UMAC_FLAGS) -c -o $@ $<

endif

$(EMU_LIB): $(EMU_OBJS) $(ZLIB_OBJS)
	@echo "  AR    $@"
	@rm -f $@
	@$(AR) cr $@ $(EMU_OBJS) $(ZLIB_OBJS)

# The web UI's pages are compiled in. Regenerate them when they change.
WEBUI_ASSETS = $(shell find src/webui/assets -type f)
src/webui/webui_assets.c: $(WEBUI_ASSETS) tools/gen_webui_assets.py
	@echo "  GEN   $@"
	@python3 tools/gen_webui_assets.py

$(TARGET).img: $(EMU_LIB)

ifneq ($(filter 4 5,$(RASPPI)),)
VIDEOCORE_LIBS :=
else
VIDEOCORE_LIBS := \
	$(CIRCLEHOME)/addon/vc4/vchiq/libvchiq.a \
	$(CIRCLEHOME)/addon/vc4/interface/bcm_host/libbcm_host.a \
	$(CIRCLEHOME)/addon/vc4/interface/khronos/libkhrn_client.a \
	$(CIRCLEHOME)/addon/vc4/interface/vmcs_host/libvmcs_host.a \
	$(CIRCLEHOME)/addon/vc4/interface/vcos/libvcos.a \
	$(CIRCLEHOME)/addon/linux/liblinuxemu.a
endif

LIBS := $(EMU_LIB) \
	$(CIRCLE_STDLIB_INSTALL_DIR)/lib/libm.a \
	$(CIRCLE_STDLIB_INSTALL_DIR)/lib/libc.a \
	$(CIRCLE_STDLIB_INSTALL_DIR)/lib/libcirclenewlib.a \
	$(CIRCLEHOME)/addon/SDCard/libsdcard.a \
	$(CIRCLEHOME)/lib/usb/libusb.a \
	$(CIRCLEHOME)/lib/input/libinput.a \
	$(CIRCLEHOME)/addon/fatfs/libfatfs.a \
	$(CIRCLEHOME)/lib/fs/libfs.a \
	$(CIRCLEHOME)/lib/net/libnet.a \
	$(CIRCLEHOME)/addon/wlan/hostap/wpa_supplicant/libwpa_supplicant.a \
	$(CIRCLEHOME)/addon/wlan/libwlan.a \
	$(VIDEOCORE_LIBS) \
	$(CIRCLEHOME)/lib/sound/libsound.a \
	$(CIRCLEHOME)/lib/sched/libsched.a \
	$(CIRCLEHOME)/lib/libcircle.a

EXTRACLEAN += $(OBJS) src/*.o src/port/*.o src/port/*.d src/port/ii/*.o src/port/ii/*.d \
              src/port/plus/*.o src/port/plus/*.d src/*.d src/webui/*.d src/sdcard/*.d \
              src/av/*.o src/av/*.d src/kms/common/*.o src/kms/common/*.d \
              src/kms/pi4/*.o src/kms/pi4/*.d \
              src/kms/pi5/*.o src/kms/pi5/*.d src/v3d/*.o src/v3d/*.d $(EMU_LIB)
.PHONY: distclean

distclean: clean
	rm -rf $(BUILD)

-include $(DEPS)

endif
