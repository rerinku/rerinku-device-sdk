# Rerinku Device SDK
#
#   make            build the SDK, examples and tests
#   make test       build + run the unit tests
#   make example    build + run the C device example: a generated test clip
#                   pushed on demand (what a camera firmware does)
#   make rebuild    clean + build
#   make clean      wipe build/

# `make example`: the platform it registers with, the serial number it uses and
# the generated clip's frame rate.
ENDPOINT   ?= http://127.0.0.1:8080
EXAMPLE_SN ?= SDK-EXAMPLE-001
FPS        ?= 25
FFMPEG     ?= ffmpeg
VIDEO_DIR  := video

.PHONY: default
default: build

.PHONY: build
build: _cmake
	@cmake --build build

.PHONY: test
test: build
	@ctest --test-dir build --output-on-failure

.PHONY: rebuild
rebuild: clean build

.PHONY: clean
clean:
	rm -rf build

# Use Ninja if present, else the platform default.
GENERATOR := $(shell command -v ninja >/dev/null 2>&1 && echo Ninja)

.PHONY: _cmake
_cmake:
	@if [ ! -f build/CMakeCache.txt ]; then \
		echo "[cmake] configuring..."; \
		cmake -S . -B build \
			$(if $(GENERATOR),-G "$(GENERATOR)",) \
			-DCMAKE_BUILD_TYPE=Release; \
	fi

# A 10 s test pattern (H.264) + tone (G.711 A-law) + thumbnail, replayed in a
# loop whenever the platform asks for the stream. Open the device in Rerinku
# View to watch it.
EXAMPLE_MEDIA := $(VIDEO_DIR)/clip.h264 $(VIDEO_DIR)/clip.alaw $(VIDEO_DIR)/thumb.jpg

$(VIDEO_DIR)/clip.h264:
	@mkdir -p $(VIDEO_DIR)
	@$(FFMPEG) -y -loglevel error -f lavfi -i testsrc2=size=640x360:rate=$(FPS) \
		-t 10 -c:v libx264 -profile:v baseline -g $$(( $(FPS) * 2 )) \
		-bsf:v h264_mp4toannexb -f h264 $@

$(VIDEO_DIR)/clip.alaw:
	@mkdir -p $(VIDEO_DIR)
	@$(FFMPEG) -y -loglevel error -f lavfi -i sine=frequency=440:sample_rate=8000 \
		-t 10 -f alaw -ar 8000 -ac 1 $@

$(VIDEO_DIR)/thumb.jpg:
	@mkdir -p $(VIDEO_DIR)
	@$(FFMPEG) -y -loglevel error -f lavfi -i testsrc2=size=640x360 -frames:v 1 $@

.PHONY: example
example: build $(EXAMPLE_MEDIA)
	@./build/rerinku_device_example --endpoint $(ENDPOINT) --sn $(EXAMPLE_SN) \
		--video $(VIDEO_DIR)/clip.h264 --fps $(FPS) \
		--audio $(VIDEO_DIR)/clip.alaw --jpeg $(VIDEO_DIR)/thumb.jpg
