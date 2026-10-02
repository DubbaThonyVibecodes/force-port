# Cross-compiles portrebind for Windows from Linux with mingw-w64.
# Command & Conquer 3 is a 32-bit game, so everything here is 32-bit too:
# a DLL can only be loaded into a process of the same bitness.

CC      := i686-w64-mingw32-gcc
CFLAGS  := -O2 -std=gnu99 -Wall -Wextra -Wno-missing-field-initializers -Wno-unused-parameter \
           -DUNICODE -D_UNICODE -D_WIN32_WINNT=0x0601
# -static: no libgcc/libwinpthread DLLs to ship.  -s: strip symbols.
LDFLAGS := -static -s
OUT     := dist

MINHOOK     := third_party/minhook
MINHOOK_SRC := $(MINHOOK)/src/hook.c $(MINHOOK)/src/buffer.c \
               $(MINHOOK)/src/trampoline.c $(MINHOOK)/src/hde/hde32.c

SHARED_SRC  := src/common.c src/inject.c
HEADERS     := src/common.h src/inject.h src/dialog.h src/resource.h
LAUNCHER    := src/launcher.c src/dialog.c
RESOURCES   := build/launcher.res.o

all: $(OUT)/portrebind.exe $(OUT)/portrebind.dll $(OUT)/portrebind.ini

# -mwindows: a windowed program, no console window pops up when it starts.
$(OUT)/portrebind.exe: $(LAUNCHER) $(SHARED_SRC) $(HEADERS) $(RESOURCES) | $(OUT)
	$(CC) $(CFLAGS) -mwindows -o $@ $(LAUNCHER) $(SHARED_SRC) $(RESOURCES) $(LDFLAGS) \
		-liphlpapi -lws2_32 -lshell32 -lcomctl32

# The dialog layout and the manifest, compiled into an object file.
$(RESOURCES): src/launcher.rc src/launcher.manifest src/resource.h
	mkdir -p build
	$(CC:gcc=windres) -Isrc src/launcher.rc -O coff -o $@

$(OUT)/portrebind.dll: src/dll.c $(SHARED_SRC) $(HEADERS) $(MINHOOK_SRC) | $(OUT)
	$(CC) $(CFLAGS) -I$(MINHOOK)/include -shared -o $@ src/dll.c $(SHARED_SRC) $(MINHOOK_SRC) \
		$(LDFLAGS) -liphlpapi -lws2_32

$(OUT)/portrebind.ini: portrebind.ini | $(OUT)
	cp $< $@

# A little program that does what the game does wrong; see test/run-wine-test.sh
$(OUT)/testapp.exe: test/testapp.c | $(OUT)
	$(CC) $(CFLAGS) -o $@ $< $(LDFLAGS) -liphlpapi -lws2_32

$(OUT):
	mkdir -p $@

test: all $(OUT)/testapp.exe
	test/run-wine-test.sh

clean:
	rm -rf $(OUT) build

.PHONY: all test clean
