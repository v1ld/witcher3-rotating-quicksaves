# Builds RotatingQuicksaves.asi with MinGW-w64 (Linux, macOS, or MSYS2 on Windows).
CC = x86_64-w64-mingw32-gcc
CFLAGS = -O2 -Wall -Wextra
LDFLAGS = -shared -s -static-libgcc

RotatingQuicksaves.asi: src/RotatingQuicksaves.c
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $<

clean:
	rm -f RotatingQuicksaves.asi
.PHONY: clean
