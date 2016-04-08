locale_csdir = $(datadir)/locale/cs/LC_MESSAGES
dist_locale_cs_DATA = $(PACKAGE_TARNAME).mo

%.mo: %.po
	msgfmt -o $@ $<
