CC ?= cc
PKG_CONFIG ?= pkg-config
CPPFLAGS += $(shell $(PKG_CONFIG) --cflags libavdevice libavformat libavcodec libavutil libswscale)
CFLAGS ?= -O2
CFLAGS += -std=c11 -Wall -Wextra -Wpedantic
LDLIBS += $(shell $(PKG_CONFIG) --libs libavdevice libavformat libavcodec libavutil libswscale)

.PHONY: all clean

all: video-recorder

video-recorder: recorder.c
	$(CC) $(CPPFLAGS) $(CFLAGS) $< $(LDLIBS) -o $@

clean:
	$(RM) video-recorder
