all : flash

TARGET:=main

TARGET_MCU?=CH32V002

# Generate test_mod.h from test.mod
test_mod.h: test.mod
	xxd -i test.mod > test_mod.h
	sed -i 's/^unsigned char test_mod\[\]/const unsigned char test_mod[]/' test_mod.h
	sed -i 's/^unsigned int test_mod_len/const unsigned int test_mod_len/' test_mod.h

# Ensure test_mod.h is generated before compiling main.c
$(TARGET).c: test_mod.h

include ch32fun/ch32fun/ch32fun.mk

flash : cv_flash

clean : cv_clean clean_mod

clean_mod:
	rm -f test_mod.h
