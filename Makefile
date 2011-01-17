# I know nothing about scons, waf, or autoconf. Sorry.
NODE_PREFIX := $(shell node --vars | egrep ^NODE_PREFIX: | cut -c14-)
NODE_PLATFORM := $(shell node --vars | egrep -o 'DPLATFORM="[^"]+' | cut -c12-)
NODE_BITS := $(shell file `which node` | egrep -o '[0-9]{2}-bit' | cut -c-2)

CPPFLAGS = -Wall -I$(NODE_PREFIX)/include -I$(NODE_PREFIX)/include/node
ifdef DEBUG
  CPPFLAGS += -ggdb -O0
else
  CPPFLAGS += -g -O3
endif

ifeq ($(NODE_BITS), 32)
  CPPFLAGS += -m32
endif
ifeq ($(NODE_BITS), 64)
  CPPFLAGS += -m64
endif

ifeq ($(NODE_PLATFORM), linux)
  CPP_DYFLAGS = -fPIC -shared
  CPP_NODEFLAGS = -fPIC -shared -Wl,-Bdynamic
  COROUTINE_SO = coroutine.so
endif
ifeq ($(NODE_PLATFORM), darwin)
  CPP_DYFLAGS = -dynamiclib
  CPP_NODEFLAGS = -bundle -undefined dynamic_lookup
  COROUTINE_SO = coroutine.dylib
endif
COROUTINE_SO_FULL := $(shell echo `pwd`/$(COROUTINE_SO))

all: $(COROUTINE_SO) node-fibers.node

$(COROUTINE_SO_FULL): $(COROUTINE_SO)

$(COROUTINE_SO): coroutine.cc
	$(CXX) $(CPPFLAGS) $(CPP_DYFLAGS) -o $@ $^ -lpthread

node-fibers.node: node-fibers.cc $(COROUTINE_SO_FULL)
	$(CXX) $(CPPFLAGS) $(CPP_NODEFLAGS) -o $@ $^

clean:
	-$(RM) node-fibers.node $(COROUTINE_SO)
	-$(RM) -r *.dSYM
