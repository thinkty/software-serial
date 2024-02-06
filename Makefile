# See https://www.kernel.org/doc/Documentation/kbuild/modules.txt

KERNEL_SRC ?= /lib/modules/$(shell uname -r)/build
MODULE_SRC := $(shell pwd)

DT_OVERLAY := soft_serial

all: dt
	$(MAKE) -C $(KERNEL_SRC) M=$(MODULE_SRC) modules

module:
	$(MAKE) -C $(KERNEL_SRC) M=$(MODULE_SRC) modules

dt: $(DT_OVERLAY).dts
	dtc -@ -I dts -O dtb -o $(DT_OVERLAY).dtbo $(DT_OVERLAY).dts

# For Clangd intellisense
bear:
	$(MAKE) clean; bear -- make module

clean:
	$(MAKE) -C $(KERNEL_SRC) M=$(MODULE_SRC) clean
	rm -f $(DT_OVERLAY).dtbo

.PHONY: all module dt modules_install bear clean
