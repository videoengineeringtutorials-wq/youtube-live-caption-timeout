CXX ?= g++
CXXFLAGS ?= -O2 -std=c++17 -Wall -Wextra -pthread
FFMPEG_FLAGS ?= $(shell pkg-config --cflags --libs libavformat libavcodec libavutil libavfilter 2>/dev/null)
CAPTION_FLAGS := $(shell pkg-config --cflags --libs libavformat libavcodec libavutil)
BUILD_DIR := .build
HEADERS := captions_608.h media_filter.h packet_reader.h flv_pacer.h youtube_logging.h

.PHONY: all check check-unit check-integration check-deps
all: cc_injector_youtube_timeout

check-deps:
	@test -n "$(strip $(FFMPEG_FLAGS))" || { echo "Install the FFmpeg development packages listed in README.md (including libavfilter-dev)." >&2; exit 1; }

cc_injector_youtube_timeout: cc_injector_youtube_timeout.cpp $(HEADERS) | check-deps
	$(CXX) $(CXXFLAGS) $< $(FFMPEG_FLAGS) -o $@

$(BUILD_DIR):
	mkdir -p $@

$(BUILD_DIR)/check_captions: tests/check_captions.cpp captions_608.h | $(BUILD_DIR)
	$(CXX) $(CXXFLAGS) $< $(CAPTION_FLAGS) -o $@

$(BUILD_DIR)/check_flv_pacer: tests/check_flv_pacer.cpp flv_pacer.h | $(BUILD_DIR)
	$(CXX) $(CXXFLAGS) $< $(CAPTION_FLAGS) -o $@

check-unit: $(BUILD_DIR)/check_captions $(BUILD_DIR)/check_flv_pacer
	$(BUILD_DIR)/check_captions
	$(BUILD_DIR)/check_flv_pacer

check-integration: cc_injector_youtube_timeout
	python3 tests/check_youtube_timeout.py

check: check-unit check-integration
