# Browsered inside sbcl-rasp: the finished editor (Browsered.html) and the
# small native helper that serves it and connects it to the rest of
# sbcl-rasp.  On macOS and Linux, make builds the helper directly; on other
# systems, or to cross-build the Windows helper, see windows/build.sh (make
# windows).  To change the editor itself, work in
# https://github.com/FemtoEmacs/browsered (rebuilding it needs Emscripten).

UNAME := $(shell uname -s)
APP := Browsered.app
LINUX_DIR := Browsered-linux
# Homebrew prefix, if any, for libcmark-gfm
BREW := $(shell brew --prefix 2>/dev/null)
LIBDIRS := $(if $(BREW),-I$(BREW)/include -L$(BREW)/lib)
# Homebrew splits the GFM extensions into a second library; link it when present
EXTLIB := $(if $(wildcard $(BREW)/lib/libcmark-gfm-extensions.*),-lcmark-gfm-extensions)
RESOURCES := Browsered.html keys.js scheme.js editor.scm init.scm \
    femtoscheme/wasm/femtoscheme.js assistant-schema.json formatting.xml \
    browser-tests.html browser-tests.js USER-TOOLS.md

.PHONY: all check clean windows

ifeq ($(UNAME),Darwin)
all: $(APP)/Contents/MacOS/browsered
else ifeq ($(UNAME),Linux)
all: $(LINUX_DIR)/bin/browsered
else
all:
	@echo "Browsered: open browsered/Browsered.html in Chrome or Edge (the helper is built on macOS, Linux and Windows)."
endif

$(APP)/Contents/MacOS/browsered: browsered.c assistant.c runtime-policy.h sources.c pdf.c $(RESOURCES)
	mkdir -p $(APP)/Contents/MacOS $(APP)/Contents/Resources
	cc -std=c11 -Wall -Wextra -Werror -O2 browsered.c assistant.c sources.c pdf.c \
	    -framework CoreFoundation -framework CoreGraphics -framework CoreText -framework PDFKit \
	    $(LIBDIRS) -lxml2 -lcmark-gfm $(EXTLIB) -o $@
	cp $(RESOURCES) $(APP)/Contents/Resources/

$(LINUX_DIR)/bin/browsered: browsered.c assistant.c runtime-policy.h sources.c $(RESOURCES)
	mkdir -p $(LINUX_DIR)/bin $(LINUX_DIR)/Resources
	cc -std=c11 -D_GNU_SOURCE -Wall -Wextra -Wno-format-truncation -Werror -O2 \
	    browsered.c assistant.c sources.c -o $@
	cp $(RESOURCES) $(LINUX_DIR)/Resources/

windows:
	cd windows && ./build.sh

check:
	@echo "Browsered: browser tests are in browser-tests.html (open it in Chrome)."

clean:
	rm -rf $(APP) $(LINUX_DIR) windows/stage windows/build
