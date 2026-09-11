############################################################################
# D13x Hengshan-Pi home control panel
############################################################################

include $(APPDIR)/Make.defs

PROGNAME  = home_panel
PRIORITY  = $(CONFIG_D13X_HOME_PANEL_PRIORITY)
STACKSIZE = $(CONFIG_D13X_HOME_PANEL_STACKSIZE)
MODULE    = $(CONFIG_D13X_HOME_PANEL)

CFLAGS += -I$(APPDIR)/industry/d13x_home_panel/ui

MAINSRC = home_panel_main.c
CSRCS   = home_panel_https.c home_panel_mijia_client.c home_panel_mijia_model.c \
	  home_panel_proactive.c \
	  home_panel_proactive_controller.c \
	  ui/home_ui_theme.c \
	  ui/home_ui_components.c \
	  ui/page_home.c \
	  ui/page_rooms.c \
	  ui/page_proactive.c \
	  ui/page_settings.c \
	  ui/page_scenes.c \
	  home_panel_font.c \
	  home_panel_misans_18.c \
	  home_panel_digits_28.c

include $(APPDIR)/Application.mk
