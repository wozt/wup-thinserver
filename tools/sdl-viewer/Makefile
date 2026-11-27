CC ?= cc
CFLAGS ?= -O2 -Wall -Wextra
PKGS = sdl2 libavcodec libavutil

wup_view: wup_view.c
	$(CC) $(CFLAGS) -o $@ $< $(shell pkg-config --cflags --libs $(PKGS)) -lm

clean:
	rm -f wup_view

.PHONY: clean
