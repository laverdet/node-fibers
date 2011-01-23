include src/platform.mk
COROUTINE_SO_FULL := $(shell echo `pwd`/src/$(COROUTINE_SO))
FIBERS_SO := $(shell echo `pwd`/src/fibers.node)

all: $(COROUTINE_SO_FULL) $(FIBERS_SO)

dist: man

$(COROUTINE_SO_FULL):
	$(MAKE) -C src $(COROUTINE_SO)

$(FIBERS_SO):
	$(MAKE) -C src fibers.node

man: README.md
	mkdir -p man
	ronn --roff $< > $@/fibers.1

clean:
	$(MAKE) -C src clean
	$(RM) -r man

.PHONY: clean $(COROUTINE_SO_FULL) $(FIBERS_SO)
