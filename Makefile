TARGET      := doom
BIN_DIR     := bin
OBJ_DIR     := $(BIN_DIR)/obj/doom

# Prefer the x86_64-elf cross compiler when installed (needed on macOS, where gcc is clang)
CC 			:= $(shell command -v x86_64-elf-gcc >/dev/null 2>&1 && echo x86_64-elf-gcc || echo gcc)

LIB_DIR     := sysroot/lib
CRT0        := $(LIB_DIR)/start.o
LIBC        := $(LIB_DIR)/blibc.a

BLIBC_REPO  := https://github.com/Bleed-Kernel/blibc.git
BLIBC_DIR   := external/blibc


CRIME_REPO  := https://github.com/Bleed-Kernel/Crime-Scene.git

CRIME_DIR   ?= $(if $(wildcard ../Crime-Scene/Makefile),../Crime-Scene,external/crime-scene)
CRIME_OUT   := libcrime
CRIME_INC   := $(CRIME_OUT)/include
LIBCRIME    := $(CRIME_OUT)/libcrime.a

# crime goes last so nothing of its can shadow a libc header
INCLUDES    := -Isysroot/include -Isysroot/include/bleed -I$(CRIME_INC)

CFLAGS := \
	-std=gnu17 \
	-ffreestanding \
	-fno-stack-protector \
	-fno-stack-check \
	-Wall -Wextra \
	-O2 \
	-m64 \
	-nostdlib \
	-nostdinc \
	-no-pie \
	$(INCLUDES)

LDFLAGS := \
	-static \
	-nostdlib \
	-no-pie \
	-m64 \
	-L$(LIB_DIR)

DOOM_FILES := \
	dummy.c am_map.c doomdef.c doomstat.c dstrings.c d_event.c d_items.c d_iwad.c \
	d_loop.c d_main.c d_mode.c d_net.c f_finale.c f_wipe.c g_game.c hu_lib.c \
	hu_stuff.c info.c i_cdmus.c i_endoom.c i_joystick.c i_scale.c i_sound.c \
	i_system.c i_timer.c memio.c m_argv.c m_bbox.c m_cheat.c m_config.c \
	m_controls.c m_fixed.c m_menu.c m_misc.c m_random.c p_ceilng.c p_doors.c \
	p_enemy.c p_floor.c p_inter.c p_lights.c p_map.c p_maputl.c p_mobj.c \
	p_plats.c p_pspr.c p_saveg.c p_setup.c p_sight.c p_spec.c p_switch.c \
	p_telept.c p_tick.c p_user.c r_bsp.c r_data.c r_draw.c r_main.c r_plane.c \
	r_segs.c r_sky.c r_things.c sha1.c sounds.c statdump.c st_lib.c st_stuff.c \
	s_sound.c tables.c v_video.c wi_stuff.c w_checksum.c w_file.c w_main.c \
	w_wad.c z_zone.c w_file_stdc.c i_input.c i_video.c doomgeneric.c doomgeneric_bleed.c \
	main.c

DOOM_SRCS := $(addprefix doomgeneric/,$(DOOM_FILES))
DOOM_OBJS := $(patsubst doomgeneric/%.c,$(OBJ_DIR)/%.o,$(DOOM_SRCS))

all: blibc crime $(BIN_DIR)/$(TARGET)

$(BIN_DIR)/$(TARGET): $(CRT0) $(DOOM_OBJS) $(LIBCRIME)
	@mkdir -p $(BIN_DIR)
	$(CC) $(LDFLAGS) \
		-o $@ \
		$(CRT0) \
		$(DOOM_OBJS) \
		$(LIBCRIME) \
		-l:blibc.a

$(OBJ_DIR)/%.o: doomgeneric/%.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@

blibc: $(LIBC)

# the sysroot has to be whole before anything compiles against it, without this a parallel
# make on a fresh clone races the copy
$(DOOM_OBJS): | crime $(LIBC)

$(CRT0): $(LIBC)

# a checkout handed in with CRIME_DIR is left as it is, only our own clone gets pulled.
# cp -p keeps the headers times so a pull that changed nothing doesnt rebuild anything
crime: | $(LIBC)
	@echo "[CRIME] Preparing crime-scene"
	@if [ ! -d "$(CRIME_DIR)" ]; then \
		git clone $(CRIME_REPO) $(CRIME_DIR); \
	elif [ "$(CRIME_DIR)" = "external/crime-scene" ]; then \
		git -C $(CRIME_DIR) pull --ff-only || echo "[CRIME] couldnt pull, building whats there"; \
	fi
	@$(MAKE) -C $(CRIME_DIR) bin/libcrime.a BLIBC_DIR=$(abspath $(BLIBC_DIR))
	@echo "[CRIME] Syncing $(CRIME_OUT)"
	@rm -rf $(CRIME_OUT)/include
	@mkdir -p $(CRIME_OUT)/include/crime
	@cp -p $(CRIME_DIR)/include/crime/*.h $(CRIME_OUT)/include/crime/
	@cmp -s $(CRIME_DIR)/bin/libcrime.a $(LIBCRIME) || cp -p $(CRIME_DIR)/bin/libcrime.a $(LIBCRIME)

$(LIBCRIME): | crime ;

$(LIBC):
	@echo "[BLIBC] Preparing blibc"
	@if [ ! -d "$(BLIBC_DIR)" ]; then \
		git clone $(BLIBC_REPO) $(BLIBC_DIR); \
	else \
		cd $(BLIBC_DIR) && git pull --rebase; \
	fi
	$(MAKE) -C $(BLIBC_DIR)
	@echo "[BLIBC] Syncing sysroot"
	@mkdir -p sysroot
	@cp -r $(BLIBC_DIR)/sysroot/* sysroot/

clean:
	rm -rf $(BIN_DIR)

distclean:
	rm -rf $(BIN_DIR) sysroot external $(CRIME_OUT)

.PHONY: all clean distclean blibc crime
