# open-ethernet trial guard — SPLICE INTO the stock rootfs init script
#   $FS/rom/etc/init.d/bcm-base-drivers.sh
# immediately after the `start)` case label (stock line ~54), BEFORE any of the
# datapath insmods (bdmf/rdpa_gpl/rdpa/.../bcm_enet/pktrunner, stock lines 88-159)
# and before the `/rom/etc/rdpa_init.sh` call (stock line 219).
#
# When the arm-flag /data/open_enet exists, this SKIPS the entire stock datapath
# stack and runs our open Runner takeover loader instead, then exits the script.
# When the flag is absent the stock path runs byte-for-byte unchanged, so the
# same rootfs boots normally — trivially revertible by `rm /data/open_enet`.

		# --- open-ethernet trial hook (revertible: rm /data/open_enet) ---
		if [ -f /data/open_enet ] && [ -x /usr/lib/open-enet/load.sh ]; then
			echo "open-enet: arm-flag set -> SKIP stock datapath, run open Runner takeover"
			/usr/lib/open-enet/load.sh
			exit 0
		fi
		# --- end open-ethernet trial hook ---
