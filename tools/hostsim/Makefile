#
# Builds the PC test harness for the emulator ports (see host_sim.c): one
# simulator for each of BMMacintosh's kernels, build/macii_sim (Basilisk
# II) and build/macplus_sim (umac), with the same front end.
#
#   make -C tools/hostsim
#   tools/hostsim/run_tests.sh
#

ROOT := ../..
SRCROOT := ../../
ZLIB := $(ROOT)/third_party/zlib
include $(ROOT)/src/port/ii/basilisk.mk
include $(ROOT)/src/port/plus/umac.mk

ZLIB_SRCS = adler32.c crc32.c inffast.c inflate.c inftrees.c zutil.c
ZLIB_CFLAGS = -DHAVE_UNISTD_H -DHAVE_STDARG_H -w

CC ?= cc
CXX ?= c++
HOSTCC ?= cc
HOSTCXX ?= c++
OUT := build
GEN := $(OUT)/gen

CFLAGS = -O2 -g
WARN = -Wall -Wextra -Wno-unused-parameter -Wno-sign-compare -Wno-missing-field-initializers

# The front end both share.
COMMON_SRCS = frontend.c keymap.c menu.c machine.c osd.c roms.c
COMMON_OBJS := $(addprefix $(OUT)/port/,$(COMMON_SRCS:.c=.o)) \
               $(addprefix $(OUT)/zlib/,$(ZLIB_SRCS:.c=.o)) \
               $(OUT)/host_sim.o $(OUT)/live.o

# ---- Basilisk II ----------------------------------------------------------------

B2_OBJS := $(addprefix $(OUT)/b2/,$(B2_CORE_SRCS:.cpp=.o)) \
           $(addprefix $(GEN)/,$(B2_GEN_SRCS:.cpp=.o)) \
           $(foreach n,$(B2_CPUEMU_PARTS),$(GEN)/cpuemu$(n).o) \
           $(addprefix $(OUT)/port/ii/,$(B2_PORT_SRCS:.cpp=.o)) \
           $(addprefix $(OUT)/slirp/,$(B2_SLIRP_SRCS:.c=.o)) \
           $(addprefix $(OUT)/slirp/,$(B2_SLIRP_PORT_SRCS:.c=.o))
B2_FLAGS = $(CFLAGS) $(B2_CXXFLAGS) $(B2_INCLUDES) -I $(GEN)

# ---- umac -----------------------------------------------------------------------

UMAC_OBJS := $(addprefix $(OUT)/umac/,$(UMAC_CORE_SRCS:.c=.o)) \
             $(addprefix $(OUT)/musashi/,$(MUSASHI_SRCS:.c=.o)) \
             $(addprefix $(GEN)/,$(UMAC_GEN_SRCS:.c=.o)) \
             $(addprefix $(OUT)/port/plus/,$(UMAC_PORT_SRCS:.c=.o))
UMAC_FLAGS = $(CFLAGS) $(UMAC_CFLAGS) $(UMAC_INCLUDES) -I $(GEN)

all: $(OUT)/macii_sim $(OUT)/macplus_sim

$(OUT)/macii_sim: $(COMMON_OBJS) $(B2_OBJS)
	$(CXX) -o $@ $^ -lm

$(OUT)/macplus_sim: $(COMMON_OBJS) $(UMAC_OBJS)
	$(CC) -o $@ $^ -lm

# Basilisk II's CPU tables and opcode handlers, as its build makes them
# (Unix/Makefile.in): build68k and gencpu are built for this computer.
$(GEN)/build68k: $(B2)/$(B2_UAE)/build68k.c
	@mkdir -p $(GEN)
	$(HOSTCC) $(B2_INCLUDES) -o $@ $<

$(GEN)/cpudefs.cpp: $(GEN)/build68k $(B2)/$(B2_UAE)/table68k
	$(GEN)/build68k < $(B2)/$(B2_UAE)/table68k > $@

# uae_cpu_2021's gencpu.c is C++; uae_cpu's is C.
ifeq ($(B2_UAE),uae_cpu_2021)
GENCPU_LANG = -x c++
else
GENCPU_LANG = -x c
endif

$(GEN)/gencpu: $(GEN)/cpudefs.cpp $(B2)/$(B2_UAE)/gencpu.c $(B2)/$(B2_UAE)/readcpu.cpp
	$(HOSTCXX) $(B2_DEFINES) $(B2_INCLUDES) -I $(GEN) -w -c -o $(GEN)/gencpu.o \
	    $(GENCPU_LANG) $(B2)/$(B2_UAE)/gencpu.c
	$(HOSTCXX) $(B2_DEFINES) $(B2_INCLUDES) -I $(GEN) -w -o $@ \
	    $(GEN)/gencpu.o $(B2)/$(B2_UAE)/readcpu.cpp $(GEN)/cpudefs.cpp

# gencpu writes into the current folder.
$(GEN)/cpuemu.cpp: $(GEN)/gencpu
	cd $(GEN) && ./gencpu

$(GEN)/cpustbl.cpp $(GEN)/cpufunctbl.cpp $(GEN)/cputbl.h: $(GEN)/cpuemu.cpp

$(GEN)/cpuemu%.o: $(GEN)/cpuemu.cpp $(ROOT)/src/port/ii/sysdeps.h
	@$(CXX) $(B2_FLAGS) -DPART_$* -c -o $@ $<

$(GEN)/%.o: $(GEN)/%.cpp $(GEN)/cputbl.h $(ROOT)/src/port/ii/sysdeps.h
	@$(CXX) $(B2_FLAGS) -c -o $@ $<

$(OUT)/b2/%.o: $(B2)/%.cpp $(GEN)/cputbl.h $(ROOT)/src/port/ii/sysdeps.h
	@mkdir -p $(dir $@)
	@$(CXX) $(B2_FLAGS) -c -o $@ $<

$(OUT)/port/ii/%.o: $(ROOT)/src/port/ii/%.cpp $(ROOT)/src/port/ii/ii.h \
                    $(ROOT)/src/port/ii/sysdeps.h $(ROOT)/src/port/bmmac.h $(ROOT)/src/port/osd.h $(GEN)/cputbl.h
	@mkdir -p $(dir $@)
	$(CXX) $(B2_FLAGS) -c -o $@ $<

$(OUT)/slirp/%.o: $(B2)/slirp/%.c $(ROOT)/src/port/ii/slirp/config.h
	@mkdir -p $(dir $@)
	@$(CC) $(CFLAGS) $(B2_SLIRP_CFLAGS) -c -o $@ $<

$(OUT)/slirp/%.o: $(ROOT)/src/port/ii/slirp/%.c $(ROOT)/src/port/ii/slirp/config.h
	@mkdir -p $(dir $@)
	@$(CC) $(CFLAGS) $(B2_SLIRP_CFLAGS) -c -o $@ $<

# Musashi's opcode handlers, written by its m68kmake from m68k_in.c.
$(GEN)/m68kmake: $(MUSASHI)/m68kmake.c
	@mkdir -p $(GEN)
	$(HOSTCC) -o $@ $<

$(GEN)/m68kops.h: $(GEN)/m68kops.c

# m68kmake writes m68kops.h with it.
$(GEN)/m68kops.c: $(GEN)/m68kmake $(MUSASHI)/m68k_in.c
	$(GEN)/m68kmake $(GEN) $(MUSASHI)/m68k_in.c

$(GEN)/m68kops.o: $(GEN)/m68kops.c
	@$(CC) $(UMAC_FLAGS) -c -o $@ $<

$(OUT)/umac/main.o: UMAC_FLAGS += $(UMAC_MAIN_CFLAGS)

$(OUT)/umac/%.o: $(UMAC)/src/%.c $(GEN)/m68kops.h $(ROOT)/src/port/plus/sonydrv.h
	@mkdir -p $(dir $@)
	@$(CC) $(UMAC_FLAGS) -c -o $@ $<

$(OUT)/musashi/%.o: $(MUSASHI)/%.c $(GEN)/m68kops.h
	@mkdir -p $(dir $@)
	@$(CC) $(UMAC_FLAGS) -c -o $@ $<

$(OUT)/port/plus/%.o: $(ROOT)/src/port/plus/%.c $(ROOT)/src/port/plus/plus.h $(ROOT)/src/port/bmmac.h $(ROOT)/src/port/osd.h $(GEN)/m68kops.h
	@mkdir -p $(dir $@)
	$(CC) $(UMAC_FLAGS) -std=gnu11 $(WARN) -c -o $@ $<

# The front end.
$(OUT)/port/%.o: $(ROOT)/src/port/%.c $(ROOT)/src/port/bmmac.h \
                 $(ROOT)/src/port/osd.h $(ROOT)/src/host.h
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -std=gnu11 -I $(ZLIB) $(WARN) -c -o $@ $<

$(OUT)/zlib/%.o: $(ZLIB)/%.c
	@mkdir -p $(dir $@)
	@$(CC) $(CFLAGS) $(ZLIB_CFLAGS) -c -o $@ $<

$(OUT)/host_sim.o: host_sim.c live.h $(ROOT)/src/host.h
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -Wall -c -o $@ $<

$(OUT)/live.o: live.c live.h
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -Wall -Wextra -DLIVE_HTML=\"$(abspath live.html)\" -c -o $@ $<

clean:
	rm -rf $(OUT)

.PHONY: all clean
