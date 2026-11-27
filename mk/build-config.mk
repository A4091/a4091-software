# Record the effective build options without touching an unchanged stamp.
# Makefiles using this rule supply CONFIG_TEXT for their stamp target.
.PHONY: FORCE
FORCE:

# Apple ships GNU Make 3.81 with whole-second timestamps. Separate a
# configuration change from outputs built in the same second so a rapid
# option switch propagates through the compile and link steps.
write-config = @set -e; tmp=$$(mktemp "$@.XXXXXX"); \
	trap 'rm -f "$$tmp"' EXIT; \
	printf '%s\n' '$(subst ','"'"',$(CONFIG_TEXT))' > "$$tmp"; \
	if ! cmp -s "$$tmp" "$@"; then \
		if [ "$(MAKE_VERSION)" = 3.81 ] && [ -f "$@" ]; then sleep 1; touch "$$tmp"; fi; \
		mv -f "$$tmp" "$@"; \
	fi
