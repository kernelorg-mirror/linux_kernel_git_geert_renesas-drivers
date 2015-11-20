# SPDX-License-Identifier: GPL-2.0
subdir-y += actions
subdir-y += al
subdir-y += allwinner
subdir-y += altera
subdir-y += amd
subdir-y += amlogic
subdir-y += apm
subdir-y += arm
subdir-y += broadcom
subdir-y += cavium
subdir-y += exynos
subdir-y += freescale
subdir-y += hisilicon
subdir-y += marvell
subdir-y += mediatek
subdir-y += nvidia
subdir-y += qcom
subdir-y += realtek
subdir-y += renesas
subdir-y += rockchip
subdir-y += socionext
subdir-y += sprd
subdir-y += xilinx
subdir-y += lg
subdir-y += zte

ifeq ($(CONFIG_OF_OVERLAY),y)
dtstree		:= $(srctree)/$(src)
dtbo-y += $(patsubst $(dtstree)/%.dtso,%.dtbo, $(foreach d,$(dts-dirs), $(wildcard $(dtstree)/$(d)/*.dtso)))
always		:= $(dtbo-y)
endif
