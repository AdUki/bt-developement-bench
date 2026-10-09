# System: the board's own plumbing scripts (tools/target/btbench-{wifi,gadget,bdaddr,data}, installed
# by btbench-base). Included by the Makefile. See docs/system.md.

SYSTEM_SCRIPTS := btbench-wifi btbench-gadget btbench-bdaddr btbench-data

## ─── system ──────────────────────────────────────────────────────────────────

# Copied over the image's copies; the next image carries the same files (btbench-base takes them
# from tools/target). No service is restarted: btbench-gadget carries the link a deploy usually
# comes over, and restarting btbench-wifi drops the Wi-Fi for a reconnect. The new script runs at
# the next boot, or when you restart its service yourself.
.PHONY: deploy-scripts
deploy-scripts: ## Copy the system scripts (Wi-Fi, gadget, bdaddr, data) to the board (TARGET=)
	@$(RSYNC) $(addprefix tools/target/,$(SYSTEM_SCRIPTS)) $(TARGET):/usr/bin/
	@echo -e "Deployed $(SYSTEM_SCRIPTS) to $(TARGET):/usr/bin"
	@echo -e "$(DIM)Not restarted. e.g.  ssh $(TARGET) systemctl restart btbench-wifi$(OFF)"
