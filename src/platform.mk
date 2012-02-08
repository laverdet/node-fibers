# I know nothing about scons, waf, or autoconf. Sorry.
NODE_PREFIX := $(shell node -e "console.log(require('path').dirname(require('path').dirname(process.execPath)))")
NODE_PLATFORM := $(shell node -e "console.log(process.platform.replace('2', ''))")
NODE_BITS := $(shell node -e "console.log(process.arch.replace(/^(?:ia|x)/, ''))")

CPPFLAGS = -Wall -Wno-deprecated-declarations -I$(NODE_PREFIX)/include -I$(NODE_PREFIX)/include/node
ifdef DEBUG
	CPPFLAGS += -ggdb -O0
else
	CPPFLAGS += -g -O3
endif

ifneq ($(HOSTTYPE), arm)
	ifeq ($(NODE_BITS), )
		CPPFLAGS += -m32
	endif
	ifeq ($(NODE_BITS), 32)
		CPPFLAGS += -m32
	endif
	ifeq ($(NODE_BITS), 64)
		CPPFLAGS += -m64
	endif
endif

ifeq ($(NODE_PLATFORM), linux)
	ifeq ($(HOSTTYPE), arm)
		# SJLJ & UCONTEXT don't work on arm (?)
		CPPFLAGS += -DCORO_PTHREAD
	else
		# SJLJ in linux = hangs & segfaults
		CPPFLAGS += -DCORO_UCONTEXT
	endif
endif
ifeq ($(NODE_PLATFORM), sunos)
	# Same as Linux
	CPPFLAGS += -DCORO_UCONTEXT
endif
ifeq ($(NODE_PLATFORM), darwin)
	# UCONTEXT in os x = hangs & segfaults :(
	CPPFLAGS += -DCORO_SJLJ
endif
ifeq ($(NODE_PLATFORM), openbsd)
       CPPFLAGS += -DCORO_ASM
endif
