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
CPPFLAGS += -DCORO_SJLJ

all: $(COROUTINE_SO) fibers.node

libcoro.o: libcoro/coro.c
	$(CC) $(CFLAGS) $(CPPFLAGS) -fPIC -c -o $@ $^

$(COROUTINE_SO_FULL): $(COROUTINE_SO)

$(COROUTINE_SO): coroutine.cc libcoro.o
	$(CXX) $(CPP_DYFLAGS) $(CXXFLAGS) $(CPPFLAGS) -o $@ $^ -lpthread -ldl

fibers.node: fibers.cc $(COROUTINE_SO_FULL)
	$(CXX) $(CPP_NODEFLAGS) $(CPPFLAGS) -o $@ $^

clean:
	-$(RM) fibers.node libcoro.o $(COROUTINE_SO)
	-$(RM) -r *.dSYM
