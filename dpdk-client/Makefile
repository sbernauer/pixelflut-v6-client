CLIENT_SOURCES := pixelflut-v6-client.c image.c

PKGCONF ?= pkg-config

# Build using pkg-config variables if possible
ifneq ($(shell $(PKGCONF) --exists libdpdk && echo 0),0)
$(error "no installation of DPDK found")
endif

all: build/pixelflut-v6-client

PC_FILE := $(shell $(PKGCONF) --path libdpdk 2>/dev/null)
# -g is for debugging symbols
CFLAGS += -O3 $(shell $(PKGCONF) --cflags libdpdk MagickWand) -DIMAGICK=$(MAGICK_VERSION) -g
LDFLAGS_SHARED = $(shell $(PKGCONF) --libs libdpdk MagickWand)
MAGICK_VERSION=$(shell pkg-config --modversion ImageMagick | grep -E -o '^[0-9]+')

CFLAGS += -DALLOW_EXPERIMENTAL_API

build/pixelflut-v6-client: $(CLIENT_SOURCES) Makefile $(PC_FILE) | build
	$(CC) $(CFLAGS) $(CLIENT_SOURCES) -o $@ $(LDFLAGS) $(LDFLAGS_SHARED)

build:
	@mkdir -p build

clean:
	rm -rf build/
