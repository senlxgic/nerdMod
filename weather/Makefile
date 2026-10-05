# nerdMod Weather: BlocksDS build (DSWiFi in DSi mode + Mbed TLS). Built in its own CI lane (see README.md).
BLOCKSDS	?= /opt/blocksds/core
BLOCKSDSEXT	?= /opt/blocksds/external

NAME		:= weather
GAME_TITLE	:= nerdMod Weather
GAME_SUBTITLE	:=
GAME_AUTHOR	:= nerdMod
GAME_ICON	:= icon.gif

SOURCEDIRS	:= source
INCLUDEDIRS	:= ../universal/include
DEFINES		:= -DNERDMOD_WEATHER

LIBS		:= -lmbedtls -lmbedx509 -lmbedcrypto -ldswifi9 -lnds9
LIBDIRS		:= $(BLOCKSDS)/libs/dswifi \
		   $(BLOCKSDSEXT)/mbedtls \
		   $(BLOCKSDS)/libs/libnds

# DSi-only app (unit code 3) like the other nerdMod apps, so TWiLight launches it in DSi mode.
NDSTOOL_ARGS	:= -uc 3 -u 00030004 -g NMWE 01 "NERDMOD"

include $(BLOCKSDS)/sys/default_makefiles/rom_arm9/Makefile
