DEFAULT_FONT := https://raw.githubusercontent.com/bjin/ctrld-font/master/ctrld-fixed-16r.bdf

.PHONY: font_header
font_header: libs/gfx/font.h

libs/gfx/font.h:
	curl -Lo bin/font.bdf $(DEFAULT_FONT)
	awk -f utils/bdf_parser.awk bin/font.bdf > $@
