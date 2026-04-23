# Simple macOS/Linux Makefile for rbterm.
# Requires raylib installed (e.g. `brew install raylib` on macOS).
# On Linux: `sudo apt install libraylib-dev` or build from source.

UNAME_S := $(shell uname -s)

CC       ?= cc
CFLAGS   ?= -O2 -g -Wall -Wextra -Wno-unused-parameter -Wno-unused-function -std=c11
LDFLAGS  ?=
LDLIBS   ?= -lraylib -lm

ifeq ($(UNAME_S),Darwin)
  BREW_PREFIX := $(shell brew --prefix 2>/dev/null)
  ifneq ($(BREW_PREFIX),)
    CFLAGS  += -I$(BREW_PREFIX)/include -I$(BREW_PREFIX)/opt/libssh/include
    LDFLAGS += -L$(BREW_PREFIX)/lib     -L$(BREW_PREFIX)/opt/libssh/lib
  endif
  LDLIBS += -framework Cocoa -framework IOKit -framework CoreVideo -framework OpenGL \
            -framework CoreText -framework CoreGraphics -framework Foundation
  EMOJI_OBJ := src/emoji_mac.o
else
  EMOJI_OBJ := src/emoji_stub.o
endif

ifeq ($(UNAME_S),Linux)
  LDLIBS += -lutil -lpthread -ldl
endif

# libssh: brew install libssh (macOS) / apt install libssh-dev (Linux).
LDLIBS += -lssh

SRCS := src/main.c src/screen.c src/render.c src/input.c src/theme.c \
        src/sixel.c src/kitty.c src/pty_unix.c src/pty_ssh.c src/pty_dispatch.c
OBJS := $(SRCS:.c=.o) $(EMOJI_OBJ) src/fonts_embedded.o

all: rbterm

rbterm: $(OBJS)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $^ $(LDLIBS)

# Embedded palette themes: regenerated whenever any kfc/dark file
# changes. Declared as a prerequisite of theme.o so a fresh clone with
# the submodule checked out picks them up automatically.
src/themes_embedded.h: tools/gen_themes.sh $(wildcard third_party/pal/palettes/kfc/dark/*)
	./tools/gen_themes.sh

src/theme.o: src/themes_embedded.h

# Embedded fonts: a tiny .S that .incbin's every file in assets/fonts/.
# Header + asm regenerate whenever any font there changes.
src/fonts_embedded.h src/fonts_embedded.S: tools/gen_fonts.sh $(wildcard assets/fonts/*.ttf assets/fonts/*.otf assets/fonts/*.ttc)
	./tools/gen_fonts.sh

src/fonts_embedded.o: src/fonts_embedded.S
	$(CC) -c $< -o $@

src/main.o: src/fonts_embedded.h

%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@

%.o: %.m
	$(CC) $(CFLAGS) -fobjc-arc -c $< -o $@

# ---------- icon + .app bundle (macOS) ----------

tools/gen_icon: tools/gen_icon.c
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $< $(LDLIBS)

assets/icon.png: tools/gen_icon
	mkdir -p assets
	./tools/gen_icon assets/icon.png

ICONSET := assets/rbterm.iconset
ICNS    := assets/rbterm.icns

$(ICNS): assets/icon.png
	rm -rf $(ICONSET)
	mkdir -p $(ICONSET)
	sips -z 16   16   assets/icon.png --out $(ICONSET)/icon_16x16.png       >/dev/null
	sips -z 32   32   assets/icon.png --out $(ICONSET)/icon_16x16@2x.png    >/dev/null
	sips -z 32   32   assets/icon.png --out $(ICONSET)/icon_32x32.png       >/dev/null
	sips -z 64   64   assets/icon.png --out $(ICONSET)/icon_32x32@2x.png    >/dev/null
	sips -z 128  128  assets/icon.png --out $(ICONSET)/icon_128x128.png     >/dev/null
	sips -z 256  256  assets/icon.png --out $(ICONSET)/icon_128x128@2x.png  >/dev/null
	sips -z 256  256  assets/icon.png --out $(ICONSET)/icon_256x256.png     >/dev/null
	sips -z 512  512  assets/icon.png --out $(ICONSET)/icon_256x256@2x.png  >/dev/null
	sips -z 512  512  assets/icon.png --out $(ICONSET)/icon_512x512.png     >/dev/null
	sips -z 1024 1024 assets/icon.png --out $(ICONSET)/icon_512x512@2x.png  >/dev/null
	iconutil -c icns $(ICONSET) -o $(ICNS)

app: rbterm $(ICNS) Info.plist
	rm -rf rbterm.app
	mkdir -p rbterm.app/Contents/MacOS rbterm.app/Contents/Resources
	cp rbterm rbterm.app/Contents/MacOS/rbterm
	cp $(ICNS) rbterm.app/Contents/Resources/rbterm.icns
	cp Info.plist rbterm.app/Contents/Info.plist
	# Fonts are embedded into the binary via tools/gen_fonts.sh +
	# .incbin — no Resources/fonts/ folder needed at runtime.
	# Ensure LaunchServices notices the bundle
	touch rbterm.app

clean:
	rm -f $(OBJS) rbterm tools/gen_icon
	rm -rf rbterm.app $(ICONSET) $(ICNS) assets/icon.png

.PHONY: all app clean
