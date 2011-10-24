# I know nothing about scons, waf, or autoconf. Sorry.
NODE_PREFIX := $(shell echo "console.log(require('path').dirname(require('path').dirname(process.execPath)))" | node)
NODE_PLATFORM := $(shell echo "console.log(process.platform.replace('2', ''))" | node)
NODE_BITS := $(shell file `echo "console.log(process.execPath)" | node` | egrep -o '[0-9]{2}-bit' | cut -c-2)

CPPFLAGS = -Wall -Wno-deprecated-declarations -I$(NODE_PREFIX)/include -I$(NODE_PREFIX)/include/node
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
	# SJLJ in linux = hangs & segfaults
	CPPFLAGS += -DCORO_UCONTEXT
endif
ifeq ($(NODE_PLATFORM), sunos)
       CPPFLAGS += -DCORO_UCONTEXT
endif
ifeq ($(NODE_PLATFORM), darwin)
	# UCONTEXT in os x = hangs & segfaults :(
	CPPFLAGS += -DCORO_SJLJ
endif
