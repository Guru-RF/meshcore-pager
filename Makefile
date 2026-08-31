# RF.Guru MeshCore Pager
#
#   make            build the firmware (PlatformIO, RP2040)
#   make upload     flash it over USB
#   make test       build + run the host protocol self-test (no hardware)
#   make monitor    serial console
#   make clean

PIO ?= $(shell command -v pio 2>/dev/null || echo $(HOME)/.platformio/penv/bin/pio)

# The repeater tree is the reference for every wire format here. `make test`
# checks the pager against its ACTUAL sources, so it needs the checkout.
REPEATER ?= ../meshcore-repeater

CXX      ?= c++
CC       ?= cc
TESTFLAGS = -O1 -g -Wall -Wextra -Wno-unused-function
TESTINC   = -Iinclude -Ilib/meshcrypto -Ilib/ed25519 -Itools/hostshim -I$(REPEATER)/src

# The pager's own protocol code, compiled for the host against tools/hostshim.
PAGER_SRC = src/meshcore.cpp src/pagerclock.cpp tools/selftest.cpp

# The repeater's real packet/advert/identity code as the counterparty. Its
# crypto/sha256.c, crypto/aes128.c and hmac_sha256.c are deliberately NOT
# listed: they are byte-identical to lib/meshcrypto (bar an include path), so
# linking both would be a duplicate-symbol error for no added coverage.
# TweetNaCl IS listed - the repeater signs with it and the pager with
# orlp/ed25519, and the advert tests exist to prove those interoperate.
REPEATER_SRC = \
	$(REPEATER)/src/packet.c \
	$(REPEATER)/src/util.c \
	$(REPEATER)/src/identity.c \
	$(REPEATER)/src/advert.c \
	$(REPEATER)/src/crypto/tweetnacl.c \
	$(REPEATER)/src/crypto/randombytes.c

CRYPTO_SRC = \
	lib/meshcrypto/aes128.c \
	lib/meshcrypto/sha256.c \
	lib/meshcrypto/hmac_sha256.c \
	lib/ed25519/fe.c lib/ed25519/ge.c lib/ed25519/keypair.c \
	lib/ed25519/sc.c lib/ed25519/sha512.c lib/ed25519/sign.c lib/ed25519/verify.c

.PHONY: all upload test monitor clean

all:
	$(PIO) run -e pico

upload:
	$(PIO) run -e pico -t upload

monitor:
	$(PIO) device monitor

# C and C++ are compiled separately: the repeater and the vendored crypto are
# C, and feeding them to a C++ driver both warns and miscompiles TweetNaCl,
# whose const arrays are legal C and not legal C++.
TESTOBJ = $(PAGER_SRC:.cpp=.test.o) $(REPEATER_SRC:.c=.test.o) $(CRYPTO_SRC:.c=.test.o)

%.test.o: %.cpp
	$(CXX) $(TESTFLAGS) $(TESTINC) -c $< -o $@

# Vendored/third-party C: no warning policy, and TweetNaCl needs C99 or later.
%.test.o: %.c
	$(CC) -O1 -g -std=c11 -w $(TESTINC) -c $< -o $@

selftest: $(TESTOBJ)
	@test -d $(REPEATER)/src || { echo "need $(REPEATER) checked out beside this one"; exit 1; }
	$(CXX) $(TESTFLAGS) -o $@ $(TESTOBJ) -lm

test: selftest
	@rm -rf .selftest-fs
	./selftest

clean:
	$(PIO) run -e pico -t clean
	rm -rf selftest selftest.dSYM .selftest-fs
	rm -f $(TESTOBJ)
