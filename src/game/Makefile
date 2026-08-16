CELL_MK_DIR = $(CELL_SDK)/samples/mk
include $(CELL_MK_DIR)/sdk.makedef.mk

CELL_BUILD_TOOLS = GCC

PPU_AR = ppu-lv2-ar
PPU_RANLIB = ppu-lv2-ranlib
PPU_STRIP = ppu-lv2-strip
PPU_CC = ppu-lv2-gcc
PPU_CXX = ppu-lv2-g++
PPU_LD = ppu-lv2-ld
PPU_CCLD = ppu-lv2-gcc
PPU_CXXLD = ppu-lv2-g++
PPU_PRX_CCLD = ppu-lv2-gcc
PPU_PRX_CXXLD = ppu-lv2-g++
PPU_PRX_EXPORTPICKUP = ppu-lve-prx-exportpickup
PPU_PRX_STRIP = ppu-lv2-prx-strip

PPU_PRX_FLAGS = -mprx -mno-sn-ld -Os -ffunction-sections -fdata-sections -fno-builtin-printf -nodefaultlibs -std=gnu99 -Wno-shadow -Wno-unused-parameter
PPU_CFLAGS = $(PPU_PRX_FLAGS)
PPU_CXXFLAGS += $(PPU_PRX_FLAGS)
PPU_LDFLAGS += $(PPU_PRX_FLAGS)
PPU_PRX_LDFLAGS += -mprx -ffunction-sections -fdata-sections

PPU_PRX_STRIPFLAGS += --strip-debug --strip-section-header

# libc.c (shared with the VSH plugin) supplies the small C runtime used under
# -nodefaultlibs. libio exposes the pad APIs; libfs reads xpad_analog.txt and
# writes the hook diagnostic log.
PPU_SRCS = ../src/libc.c xpad_game.c
PPU_PRX_LDLIBS = -lio_stub -lfs_stub
PPU_PRX_TARGET = xpad_game.prx

CLEANFILES = $(PRX_DIR)/$(PPU_SPRX_TARGET) xpad_game_scetool.sprx

# PS3HEN needs key revision 0A. A game-process module also needs the retail
# game auth id (1010000001000003), not the VSH auth id used by xpad_vsh.
all:
	$(MAKE) $(PPU_OBJS_DEPENDS)
	$(PPU_PRX_STRIP) $(PPU_PRX_STRIPFLAGS) $(PPU_PRX_TARGET)
	scetool -0 SELF -1 TRUE -s FALSE -2 0A -3 1010000001000003 -4 01000002 -5 APP -6 0003004000000000 -A 0001000000000000 -e xpad_game.prx xpad_game_scetool.sprx

include $(CELL_MK_DIR)/sdk.target.mk
