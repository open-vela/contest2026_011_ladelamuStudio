############################################################################
# vendor/artinchip/boards/d13x-hengshan-pi/src/Makefile
#
# D13x Hengshan Pi 板级 Makefile
#
############################################################################

include $(TOPDIR)/Make.defs

CSRCS = artinchip_boot.c artinchip_bringup.c

ifeq ($(CONFIG_D13X_TOUCH_GT911),y)
  CSRCS += artinchip_touch.c
endif

ifeq ($(CONFIG_BOARDCTL),y)
  CSRCS += artinchip_appinit.c
endif

include $(TOPDIR)/boards/Board.mk

.PHONY: clean distclean

clean::
	$(call DELFILE, libboard.a)
	$(call CLEAN)

distclean:: clean
