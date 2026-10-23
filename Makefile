TARGET      := doom
BIN_DIR     := bin
OBJ_DIR     := $(BIN_DIR)/obj/doom

# Prefer the x86_64-elf cross compiler when installed (needed on macOS, where gcc is clang)
CC 			:= $(shell command -v x86_64-elf-gcc >/dev/null 2>&1 && echo x86_64-elf-gcc || echo gcc)

LIB_DIR     := sysroot/lib
CRT0        := $(LIB_DIR)/start.o
LIBC        := $(LIB_DIR)/blibc.a

BLIBC_REPO  ?= https://github.com/Bleed-Kernel/blibc.git
# a sibling checkout is used as it is, same as crime-scene and libaudio, only our own clone gets pulled
BLIBC_DIR   ?= $(if $(wildcard ../blibc/include/bleed),../blibc,external/blibc)


CRIME_REPO  := https://github.com/Bleed-Kernel/Crime-Scene.git
AUDIO_REPO  ?= https://github.com/Bleed-Kernel/libaudio.git

CRIME_DIR   ?= $(if $(wildcard ../Crime-Scene/Makefile),../Crime-Scene,external/crime-scene)
CRIME_OUT   := libcrime
CRIME_INC   := $(CRIME_OUT)/include
LIBCRIME    := $(CRIME_OUT)/libcrime.a

AUDIO_DIR   ?= $(if $(wildcard ../libaudio/Makefile),../libaudio,external/libaudio)
AUDIO_OUT   := libaudio
LIBAUDIO    := $(AUDIO_OUT)/libaudio.a

# crime goes last so nothing of its can shadow a libc header
INCLUDES    := -Isysroot/include -Isysroot/include/bleed -I$(CRIME_INC) -I$(AUDIO_OUT)/include

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
	-DFEATURE_SOUND \
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
	main.c mus2mid.c i_bleedsound.c

DOOM_SRCS := $(addprefix doomgeneric/,$(DOOM_FILES))
DOOM_OBJS := $(patsubst doomgeneric/%.c,$(OBJ_DIR)/%.o,$(DOOM_SRCS))

all: blibc crime audio $(BIN_DIR)/$(TARGET)

OBJCOPY := $(shell command -v x86_64-elf-objcopy >/dev/null 2>&1 && echo x86_64-elf-objcopy || echo objcopy)
ICONS   := $(wildcard res/icon.png res/icon-16.png res/icon-32.png res/icon-48.png)

define embed_icons
	@if [ -f res/icon.png ]; then \
		$(OBJCOPY) --add-section .bleed.icon=res/icon.png \
			--set-section-flags .bleed.icon=noload,readonly $(1) || exit 1; \
	fi
	@for px in 16 32 48; do \
		if [ -f res/icon-$$px.png ]; then \
			$(OBJCOPY) --add-section .bleed.icon$$px=res/icon-$$px.png \
				--set-section-flags .bleed.icon$$px=noload,readonly $(1) || exit 1; \
		fi; \
	done
	@if [ -f res/app.txt ]; then \
		$(OBJCOPY) --add-section .bleed.app=res/app.txt \
			--set-section-flags .bleed.app=noload,readonly $(1) || exit 1; \
	fi
endef

$(BIN_DIR)/$(TARGET): $(CRT0) $(DOOM_OBJS) $(LIBCRIME) $(LIBAUDIO) $(ICONS)
	@mkdir -p $(BIN_DIR)
	$(CC) $(LDFLAGS) \
		-o $@ \
		$(CRT0) \
		$(DOOM_OBJS) \
		$(LIBCRIME) \
		$(LIBAUDIO) \
		-l:blibc.a
	$(call embed_icons,$@)

$(OBJ_DIR)/%.o: doomgeneric/%.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@

# synced every build, a sysroot only filled in when blibc.a was missing went stale against blibc fixes
blibc: $(LIBC)
	@$(MAKE) -C $(BLIBC_DIR) --no-print-directory
	@cp -Rp $(BLIBC_DIR)/sysroot/* sysroot/

# the sysroot has to be whole before anything compiles against it, without this a parallel
# make on a fresh clone races the copy
$(DOOM_OBJS): | crime audio $(LIBC)

$(CRT0): $(LIBC)

# a checkout handed in with CRIME_DIR is left as it is, only our own clone gets pulled.
# cp -p keeps the headers times so a pull that changed nothing doesnt rebuild anything
# after blibc, they both build against it too and would be at its sysroot at the same time
crime: | blibc
	@echo "[CRIME] Preparing crime-scene"
	@if [ ! -d "$(CRIME_DIR)" ]; then \
		git clone $(CRIME_REPO) $(CRIME_DIR); \
	elif [ "$(CRIME_DIR)" = "external/crime-scene" ]; then \
		cd $(CRIME_DIR) && git pull --rebase; \
	fi
	@$(MAKE) -C $(CRIME_DIR) bin/libcrime.a BLIBC_DIR=$(abspath $(BLIBC_DIR))
	@echo "[CRIME] Syncing $(CRIME_OUT)"
	@rm -rf $(CRIME_OUT)/include
	@mkdir -p $(CRIME_OUT)/include/crime
	@cp -p $(CRIME_DIR)/include/crime/*.h $(CRIME_OUT)/include/crime/
	@cmp -s $(CRIME_DIR)/bin/libcrime.a $(LIBCRIME) || cp -p $(CRIME_DIR)/bin/libcrime.a $(LIBCRIME)

$(LIBCRIME): | crime ;

# same deal as crime, sibling checkout as it is, our own clone pulled
audio: | blibc
	@echo "[AUDIO] Preparing libaudio"
	@if [ ! -d "$(AUDIO_DIR)" ]; then \
		git clone $(AUDIO_REPO) $(AUDIO_DIR); \
	elif [ "$(AUDIO_DIR)" = "external/libaudio" ]; then \
		cd $(AUDIO_DIR) && git pull --rebase; \
	fi
	@$(MAKE) -C $(AUDIO_DIR) bin/libaudio.a BLIBC_DIR=$(abspath $(BLIBC_DIR))
	@echo "[AUDIO] Syncing $(AUDIO_OUT)"
	@rm -rf $(AUDIO_OUT)/include
	@mkdir -p $(AUDIO_OUT)/include
	@cp -p $(AUDIO_DIR)/include/bleed_audio.h $(AUDIO_OUT)/include/
	@cmp -s $(AUDIO_DIR)/bin/libaudio.a $(LIBAUDIO) || cp -p $(AUDIO_DIR)/bin/libaudio.a $(LIBAUDIO)

$(LIBAUDIO): | audio ;

# same as crime, a checkout handed in with BLIBC_DIR is left as it is, only our own clone gets pulled
$(LIBC):
	@echo "[BLIBC] Preparing blibc"
	@if [ ! -d "$(BLIBC_DIR)" ]; then \
		git clone $(BLIBC_REPO) $(BLIBC_DIR); \
	elif [ "$(BLIBC_DIR)" = "external/blibc" ]; then \
		cd $(BLIBC_DIR) && git pull --rebase; \
	fi
	$(MAKE) -C $(BLIBC_DIR)
	@echo "[BLIBC] Syncing sysroot"
	@mkdir -p sysroot
	@cp -r $(BLIBC_DIR)/sysroot/* sysroot/

clean:
	rm -rf $(BIN_DIR)

distclean:
	rm -rf $(BIN_DIR) sysroot external $(CRIME_OUT) $(AUDIO_OUT)

.PHONY: all clean distclean blibc crime audio
