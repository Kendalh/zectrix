# Host simulator for the rawdraw UI. Builds firmware sources unmodified against shims in shim/.
# Usage: make -C sim run PAGE=weather KEYS=down,down
ROOT    := $(abspath ..)
MAIN    := $(ROOT)/firmware/main
FONTS   := $(MAIN)/components/78__xiaozhi-fonts/src
BUILD   := build
PAGE    ?= gallery
KEYS    ?=

# cJSON is an ESP-IDF managed component that is not vendored here; fetch a pinned release once.
CJSON_TAG := v1.7.18
CJSON_DIR := $(BUILD)/cjson-src

DEFS := -DBUILTIN_TEXT_FONT=SourceHanSansSC_Regular_slim -DBUILTIN_ICON_FONT=font_zectrix_16_1 -DPROJECT_VER=\"6.5.9\"
INC := -Ishim -I$(CJSON_DIR) -I$(MAIN) -I$(MAIN)/rawdraw -I$(MAIN)/rawdraw/components -I$(MAIN)/ui/renderers/rawdraw -I.
CXXFLAGS := -std=c++17 -O1 -g $(INC) -Wno-attributes $(DEFS)
CFLAGS   := -O1 $(INC)

CXX_SRCS := $(wildcard $(MAIN)/rawdraw/*.cc) \
            $(filter-out %/calendar.cc,$(wildcard $(MAIN)/rawdraw/components/*.cc)) \
            $(addprefix $(MAIN)/ui/renderers/rawdraw/,photo_gallery.cc photo_detail_renderer.cc weather_renderer.cc weather_detail_renderer.cc) \
            $(addprefix $(MAIN)/ui/renderers/rawdraw/,home_renderer.cc settings_renderer.cc voice_renderer.cc) \
            $(MAIN)/ui/app_registry.cc $(MAIN)/ui/app_shell.cc \
            $(MAIN)/ui/renderers/rawdraw/ap_transfer_renderer.cc \
            $(MAIN)/ui/renderers/rawdraw/ap_transfer_server.cc \
            $(MAIN)/ui/renderers/rawdraw/web_apps.cc $(MAIN)/common/weather_parse.cc $(MAIN)/common/voice_recordings.cc \
            fake/voice_backend_fake.cc fake/photo_storage.cc fake/weather_api_fake.cc sim_httpd.cc sim_ui.cc sim_main.cc sim_device.cc
C_SRCS   := $(filter-out %/cbin_font.c,$(wildcard $(FONTS)/*.c $(FONTS)/*/*.c)) sim_platform.c $(CJSON_DIR)/cJSON.c

OBJS := $(patsubst %,$(BUILD)/%.o,$(notdir $(CXX_SRCS) $(C_SRCS)))
# fake/ first: common/photo_storage.cc (ESP-IDF) must not shadow fake/photo_storage.cc
vpath %.cc fake
vpath %.cc $(sort $(dir $(CXX_SRCS)))
vpath %.c  $(sort $(dir $(C_SRCS)))
LIBOBJS := $(filter-out $(BUILD)/sim_main.cc.o $(BUILD)/sim_device.cc.o,$(OBJS))

all: $(BUILD)/sim_main $(BUILD)/sim_device
$(BUILD)/sim_main: $(LIBOBJS) $(BUILD)/sim_main.cc.o
	$(CXX) $^ -o $@ -lpthread
$(BUILD)/sim_device: $(LIBOBJS) $(BUILD)/sim_device.cc.o
	$(CXX) $^ -o $@ -lpthread
$(OBJS): | $(CJSON_DIR)/cJSON.c
$(CJSON_DIR)/cJSON.c:
	mkdir -p $(BUILD)
	git clone -q --depth 1 --branch $(CJSON_TAG) https://github.com/DaveGamble/cJSON.git $(CJSON_DIR)
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

# Type-check firmware files the simulator cannot link (they need real ESP-IDF hardware headers).
# Uses the stubs in check/ and the shims in shim/. This is a safety net, not a build.
check-firmware: | $(CJSON_DIR)/cJSON.c
	$(CXX) -std=c++17 -fsyntax-only -Icheck $(CXXFLAGS) $(MAIN)/ui/rawdraw_ui_manager.cc
	@echo "check-firmware: rawdraw_ui_manager.cc OK (application.cc is not covered)"

$(BUILD)/test_weather_parse: tests/test_weather_parse.cc $(LIBOBJS)
	$(CXX) $(CXXFLAGS) $^ -o $@ -lpthread

test: all $(BUILD)/test_weather_parse
	$(BUILD)/test_weather_parse
	python3 tests/test_navigation.py -v
	@command -v node >/dev/null && node tests/test_buttons.js || echo "node not found: skipped tests/test_buttons.js"

clean:
	rm -rf $(BUILD) out
-include $(wildcard $(BUILD)/*.d)
.PHONY: all run test check-firmware clean
