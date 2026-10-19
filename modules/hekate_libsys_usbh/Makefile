ifeq ($(strip $(DEVKITARM)),)
$(error "Please set DEVKITARM in your environment. export DEVKITARM=<path to>devkitARM")
endif

include $(DEVKITARM)/base_rules

TARGET := libsys_usbh
BUILD := ../../build/$(TARGET)
OUTPUT := ../../output
BDKDIR := bdk
BDKINC := -I../../$(BDKDIR)
VPATH = ./ ../../$(BDKDIR)/usb/ ../../$(BDKDIR)/power/

# Module entry and bdk service shims, the XHCI host and MSC drivers, VBUS regulator control.
OBJS = $(addprefix $(BUILD)/,\
	sys_usbh.o usbh.o usbh_msc.o usbh_dock.o regulator_5v.o \
)

GFX_INC := '"../bootloader/gfx/gfx.h"'
CUSTOMDEFINES := -DGFX_INC=$(GFX_INC)

ARCH := -march=armv4t -mtune=arm7tdmi -mthumb-interwork
CFLAGS = $(ARCH) -O2 -nostdlib -fpie -ffunction-sections -fdata-sections -fomit-frame-pointer -std=gnu11 -Wall -Wsign-compare $(CUSTOMDEFINES)
LDFLAGS = $(ARCH) -fpie -pie -nostartfiles -lgcc -Wl,--gc-sections

.PHONY: clean all

all: $(TARGET).bso
$(BUILD)/%.o: %.c
	@mkdir -p "$(BUILD)"
	@$(CC) $(CFLAGS) $(BDKINC) -c $< -o $@

$(TARGET).bso: $(OBJS)
	@$(CC) $(LDFLAGS) -e _usbh_module_init $^ -o $(OUTPUT)/$(TARGET).bso
	@$(STRIP) -g $(OUTPUT)/$(TARGET).bso
	@echo  -e "-------------\nBuilt module: "$(TARGET)".bso\n-------------"

clean:
	@rm -rf $(OUTPUT)/$(TARGET).bso
