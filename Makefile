# sbcl-rasp: build and test every component from here.
#   make         build RASP and, on macOS and Linux, the Browsered helper
#   make check   run the RASP and qprolog tests
SBCL ?= sbcl

.PHONY: all rasp browsered qprolog check clean

all: rasp browsered qprolog

rasp:
	$(MAKE) -C rasp

browsered:
	$(MAKE) -C browsered

qprolog:
	@echo "qprolog: nothing to build (it is loaded by SBCL at run time)."

check: rasp
	$(MAKE) -C rasp check
	cd qprolog && $(SBCL) --dynamic-space-size 4GB --control-stack-size 512MB \
	    --non-interactive --load tests/basic.lisp

clean:
	$(MAKE) -C rasp clean
	$(MAKE) -C browsered clean

# Optional small editor: C owns the command loop and embeds femtoscheme.
.PHONY: schememax check-schememax check-schememax-ui
schememax:
	$(MAKE) -C schememax

check-schememax:
	$(MAKE) -C schememax check

check-schememax-ui:
	$(MAKE) -C schememax check-ui
