NODE_PREFIX := $(shell node --vars | egrep ^NODE_PREFIX: | cut -c14-)
# CPPFLAGS = -ggdb -O0 -Wall -I$(NODE_PREFIX)/include -I$(NODE_PREFIX)/include/node -I/Users/marcel/code/v8/include
CPPFLAGS = -m64 -ggdb -O0 -Wall -I$(NODE_PREFIX)/include -I$(NODE_PREFIX)/include/node

all: node-fibers.node

coroutine.dylib: coroutine.cc
	$(CXX) $(CPPFLAGS) -dynamiclib -o $@ $^

node-fibers.node: node-fibers.cc coroutine.dylib
	$(CXX) $(CPPFLAGS) -bundle -undefined dynamic_lookup -o $@ $^

clean:
	-$(RM) node-fibers.node coroutine.dylib
	-$(RM) -r *.dSYM
