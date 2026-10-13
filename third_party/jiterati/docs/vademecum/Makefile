# Makefile — build "Jiterati: A Vademecum" (XeLaTeX via latexmk + chktex)
#
# Targets:
#   make          build the PDF (build/Jiterati-Vademcum.pdf)
#   make check    run chktex on the source (the required lint step)
#   make view     open the PDF with xdg-open
#   make clean    remove generated aux files (keep the PDF)
#   make distclean remove the entire build/ directory, including the PDF
#
# This Makefile is a thin wrapper over latexmk (see .latexmkrc) and chktex,
# so that compilation and linting remain independent, inspectable steps.

DOC      := Jiterati-Vademcum.tex
PDF      := build/Jiterati-Vademcum.pdf
LATEXMK  := latexmk
CHKTEX   := chktex

.PHONY: all check view clean distclean

all: $(PDF)

# latexmk reads .latexmkrc in this directory for engine + paths.
$(PDF): $(DOC) .latexmkrc .chktexrc
	$(LATEXMK) $(DOC)

# The required lint step: `chktex Jiterati-Vademcum.tex`.
# chktex reads the local .chktexrc for resource paths and warning policy.
# The `-q` flag suppresses the banner so only actual warnings print.
check: $(DOC) .chktexrc
	@echo "==> chktex $(DOC)"
	@$(CHKTEX) -q $(DOC); \
	status=$$?; \
	# chktex returns a bitmask: bit 0 = errors, bit 1 = warnings. \
	# Treat errors as fatal; warnings are advisory but reported. \
	if [ $$((status & 1)) -ne 0 ]; then \
		echo "chktex reported errors (exit $$status)"; exit 1; \
	elif [ $$status -ne 0 ]; then \
		echo "chktex reported warnings (exit $$status)"; \
	else \
		echo "chktex: clean"; \
	fi

view: $(PDF)
	xdg-open $(PDF) >/dev/null 2>&1 || true

clean:
	$(LATEXMK) -c

distclean:
	$(LATEXMK) -C
	rm -rf build
