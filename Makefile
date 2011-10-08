include src/platform.mk
FIBERS_SO := $(shell echo `pwd`/src/fibers.node)

all: fibers test

fibers: $(FIBERS_SO)

dist: man

$(FIBERS_SO):
	$(MAKE) -C src fibers.node

man: README.md
	mkdir -p man
	ronn --roff $< > $@/fibers.1

test: fibers
	./test.sh

clean:
	$(MAKE) -C src clean
	$(RM) -r man

.PHONY: clean dist fibers test $(FIBERS_SO)
