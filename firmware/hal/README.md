# firmware/hal: OPLBOX's own hardware code

Register access that OPLBOX adds and felucca-bsp does not have (for example the WiFi chip's registers). These files
are OPLBOX's, GPL-3.0-only like the rest of OPLBOX. The chip and board layer OPLBOX shares with Felucca is in
`firmware/bsp/` (MPL-2.0).

`tools/build.py` puts this directory on the include path after `firmware/bsp/`, and its register check lets code
here, and in `firmware/bsp/`, touch hardware registers; code in `firmware/src/` must call a helper from one of them.
