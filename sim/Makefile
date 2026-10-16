# Host simulator for the rawdraw UI. Builds firmware sources unmodified against shims in shim/.
# Usage: make -C sim run PAGE=weather KEYS=down,down
ROOT    := $(abspath ..)
MAIN    := $(ROOT)/firmware/main
FONTS   := $(MAIN)/components/78__xiaozhi-fonts/src
BUILD   := build
PAGE    ?= gallery
KEYS    ?=

DEFS := -DBUILTIN_TEXT_FONT=SourceHanSansSC_Regular_slim -DBUILTIN_ICON_FONT=font_zectrix_16_1
INC := -Ishim -I$(MAIN) -I$(MAIN)/rawdraw -I$(MAIN)/rawdraw/components -I$(MAIN)/ui/renderers/rawdraw
CXXFLAGS := -std=c++17 -O1 -g $(INC) -Wno-attributes $(DEFS)
CFLAGS   := -O1 $(INC)

CXX_SRCS := $(wildcard $(MAIN)/rawdraw/*.cc) \
            $(filter-out %/calendar.cc,$(wildcard $(MAIN)/rawdraw/components/*.cc)) \
            $(addprefix $(MAIN)/ui/renderers/rawdraw/,photo_gallery.cc photo_detail_renderer.cc weather_renderer.cc weather_detail_renderer.cc) \
            fake/photo_storage.cc fake/weather_api.cc sim_main.cc
C_SRCS   := $(filter-out %/cbin_font.c,$(wildcard $(FONTS)/*.c $(FONTS)/*/*.c)) sim_platform.c

OBJS := $(patsubst %,$(BUILD)/%.o,$(notdir $(CXX_SRCS) $(C_SRCS)))
vpath %.cc $(sort $(dir $(CXX_SRCS)))
vpath %.c  $(sort $(dir $(C_SRCS)))

all: $(BUILD)/sim_main
$(BUILD)/sim_main: $(OBJS)
	$(CXX) $^ -o $@
$(BUILD)/%.cc.o: %.cc | $(BUILD)
	$(CXX) $(CXXFLAGS) -MMD -c $< -o $@
$(BUILD)/%.c.o: %.c | $(BUILD)
	$(CC) $(CFLAGS) -MMD -c $< -o $@
$(BUILD):
	mkdir -p $@

run: all
	mkdir -p out
	$(BUILD)/sim_main --page $(PAGE) --out out $(if $(KEYS),--keys $(KEYS)) | tee out/.last
	python3 -I tools/fb2png.py 400 300 $$(grep '\.fb$$' out/.last)

clean:
	rm -rf $(BUILD) out
-include $(wildcard $(BUILD)/*.d)
.PHONY: all run clean
