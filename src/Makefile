include platform.mk

ifeq ($(NODE_PLATFORM), linux)
	CPP_NODEFLAGS = -fPIC -shared -Wl,-Bdynamic
endif
ifeq ($(NODE_PLATFORM), sunos)
       CPP_NODEFLAGS = -fPIC -shared -Wl,-Bdynamic
endif
ifeq ($(NODE_PLATFORM), darwin)
	CPP_NODEFLAGS = -bundle -undefined dynamic_lookup
endif

all: fibers.node

libcoro.o: libcoro/coro.c
	$(CC) $(CFLAGS) $(CPPFLAGS) -fPIC -c -o $@ $^

coroutine.o: coroutine.cc
	$(CXX) $(CXXFLAGS) $(CPPFLAGS) -fPIC -c -o $@ $^

fibers.node: fibers.cc coroutine.o libcoro.o
	$(CXX) $(CPP_NODEFLAGS) $(CPPFLAGS) $^ -o $@ -lpthread

clean:
	-$(RM) fibers.node libcoro.o coroutine.o
	-$(RM) -r *.dSYM
