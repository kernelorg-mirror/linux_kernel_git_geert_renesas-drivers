dtb-$(CONFIG_ARCH_RCAR_GEN3) += r8a7795-salvator-x.dtb

ifeq ($(CONFIG_OF_OVERLAY),y)
dtb-$(CONFIG_ARCH_RCAR_GEN3) += \
	r8a7795-salvator-x-hscif1.dtbo
endif

always		:= $(dtb-y)
subdir-y	:= $(dts-dirs)
clean-files	:= *.dtb
