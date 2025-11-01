all : flash

TARGET:=main

TARGET_MCU?=CH32V006

# Select MOD file based on MCU type
ifeq ($(TARGET_MCU),CH32V006)
    MOD_FILE:=f-tube.mod
else
    MOD_FILE:=test.mod
endif

# Generate test_mod.h from selected MOD file
test_mod.h: $(MOD_FILE)
	xxd -i $(MOD_FILE) > test_mod.h
	sed -i 's/^unsigned char .*\[\]/const unsigned char test_mod[]/' test_mod.h
	sed -i 's/^unsigned int .*_len/const unsigned int test_mod_len/' test_mod.h

# Ensure test_mod.h is generated before compiling main.c
$(TARGET).c: test_mod.h

include ch32fun/ch32fun/ch32fun.mk

flash : cv_flash

clean : cv_clean clean_mod

clean_mod:
	rm -f test_mod.h
