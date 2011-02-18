include platform.mk

ifeq ($(NODE_PLATFORM), linux)
	CPP_DYFLAGS = -fPIC -shared
	CPP_NODEFLAGS = -fPIC -shared -Wl,-Bdynamic
endif
ifeq ($(NODE_PLATFORM), darwin)
	CPP_DYFLAGS = -dynamiclib
	CPP_NODEFLAGS = -bundle -undefined dynamic_lookup
endif
COROUTINE_SO_FULL := $(shell echo `pwd`/$(COROUTINE_SO))

all: $(COROUTINE_SO) fibers.node

$(COROUTINE_SO_FULL): $(COROUTINE_SO)

$(COROUTINE_SO): coroutine.cc
	$(CXX) $(CPPFLAGS) $(CPP_DYFLAGS) -o $@ $^ -lpthread -ldl

fibers.node: fibers.cc $(COROUTINE_SO_FULL)
	$(CXX) $(CPPFLAGS) $(CPP_NODEFLAGS) -o $@ $^

clean:
	-$(RM) fibers.node $(COROUTINE_SO)
	-$(RM) -r *.dSYM
