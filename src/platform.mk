# I know nothing about scons, waf, or autoconf. Sorry.
NODE_PREFIX := $(shell node --vars | egrep ^NODE_PREFIX: | cut -c14-)
NODE_PLATFORM := $(shell node --vars | egrep -o 'DPLATFORM="[^"]+' | cut -c12-)
NODE_BITS := $(shell file `which node` | egrep -o '[0-9]{2}-bit' | cut -c-2)

CPPFLAGS = -Wall -I$(NODE_PREFIX)/include -I$(NODE_PREFIX)/include/node
ifdef DEBUG
	CPPFLAGS += -ggdb -O0
else
	CPPFLAGS += -g -O3 -minline-all-stringops
endif

ifeq ($(NODE_BITS), )
	CPPFLAGS += -m32
endif
ifeq ($(NODE_BITS), 32)
	CPPFLAGS += -m32
endif
ifeq ($(NODE_BITS), 64)
	CPPFLAGS += -m64
endif

ifeq ($(NODE_PLATFORM), linux)
	COROUTINE_SO = coroutine.so
endif
ifeq ($(NODE_PLATFORM), darwin)
	COROUTINE_SO = coroutine.dylib
endif
