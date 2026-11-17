# Host unit tests for DiagCoreLib (gcc or clang), with AddressSanitizer and UBSan.
CC ?= cc
ROOT := ../..
# SAN= disables the sanitizers (for toolchains without the ASan runtime)
SAN ?= -fsanitize=address,undefined -fno-sanitize-recover=all
CFLAGS := -std=c11 -O1 -g -Wall -Wextra -Werror -Wno-unused-parameter $(SAN) -I shim -I $(ROOT)/HwDiagPkg/Include

test_diagcore: test_diagcore.c $(ROOT)/HwDiagPkg/Library/DiagCoreLib/DiagCore.c
	$(CC) $(CFLAGS) -o $@ $^

check: test_diagcore
	./test_diagcore

clean:
	rm -f test_diagcore
.PHONY: check clean
