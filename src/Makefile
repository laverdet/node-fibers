include platform.mk
COROUTINE_SO_FULL := $(shell echo `pwd`/$(COROUTINE_SO))

ifeq ($(NODE_PLATFORM), linux)
	CPP_DYFLAGS = -fPIC -shared
	CPP_NODEFLAGS = -fPIC -shared -Wl,-Bdynamic
endif
ifeq ($(NODE_PLATFORM), darwin)
	CPP_DYFLAGS = -dynamiclib
	CPP_NODEFLAGS = -bundle -undefined dynamic_lookup
endif

all: $(COROUTINE_SO_FULL) fibers.node

libcoro.o: libcoro/coro.c
	$(CC) $(CFLAGS) $(CPPFLAGS) -fPIC -c -o $@ $^

$(COROUTINE_SO_FULL): $(COROUTINE_SO)

$(COROUTINE_SO): coroutine.cc libcoro.o
	$(CXX) $(CPP_DYFLAGS) $(CXXFLAGS) $(CPPFLAGS) -o $(COROUTINE_SO_FULL) $^ -lpthread -ldl

fibers.node: fibers.cc $(COROUTINE_SO_FULL)
	$(CXX) $(CPP_NODEFLAGS) $(CPPFLAGS) $^ -o $@

clean:
	-$(RM) fibers.node libcoro.o $(COROUTINE_SO)
	-$(RM) -r *.dSYM
