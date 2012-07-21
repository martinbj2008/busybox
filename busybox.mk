-include ../../env.mk

all:
	make
prepare:
	#busybox need not configuration. 
	@sed 's:CONFIG_PREFIX="":CONFIG_PREFIX="${ROOT_ROMFS}":g' busybox.config>.config

install:
	@cp -v busybox ${ROOT_ROMFS}/bin

