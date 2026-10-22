# Native default and root-only exec file: lifecycle on the VM (2026-10-21)

**Result:** upgrading VM .2 from 0.3.17 to this build switches its instance,
which sets no `pinger_method`, to the native probes, and the service behaves
as before through every lifecycle step. Two of the script's 24 checks failed;
both were artefacts of the test, and the same cases pass when repeated
cleanly by hand (below).

Build: `cake-adapt-mqtt` and `luci-app-cake-adapt` 0.3.19-r1 from the x86
SDK, with the changes of this commit series. VM: OpenWrt 25.12 i386, the
installed instance `main` on `eth1`/`ifb4eth1` with 30 numeric reflectors,
`tcp_delay_attribution` and `tcp_ts_request` on, MQTT off.

[`lifecycle.sh`](lifecycle.sh) ran on the VM, bounded, and wrote
[`steps.txt`](steps.txt):

- **Upgrade:** fping stays (it was installed explicitly). The configuration is
  kept, and `/etc/config/cake-adapt-exec` installs with mode 0600.
- **Start:** the restarted instance runs native, with no fping child; samples
  arrive, `ubus call cake-adapt.main status` answers, and LuCI answers.
- **Reload:** with nothing changed, the instance keeps its pid.
- **Exec-file change:** a `config_file` set in the exec file restarts the
  instance. Its value (`no_pingers=5`) and `config_file` reach the runtime
  file.
- **Refusal:** `config_file` in `/etc/config/cake-adapt` is refused, and syslog
  names the exec file.
- **Fallback:** `pinger_method fping` starts an fping child and samples;
  unsetting it returns to native.
- **Stop and start** work, and the test-log inode was kept.

The two failures:

- **"kept running" after the refused `config_file`.** The pid changed, but the
  restart came from the exec-file change one step earlier. On this build
  procd's reload trigger starts its own asynchronous reload after each
  `uci commit`, and it overlapped the script's own reload; `pidof` also caught
  the short-lived `cake-adapt -V` run. The instance ended on the valid new
  configuration. Repeated by hand with nothing pending, the refused reload kept
  the same pid and an unchanged runtime file, and logged "keeps its previous
  configuration".
- **"same daemon" after killing ubusd.** A triggered reload was still running
  when ubusd was killed ("Failed to connect to ubus" from the init script),
  and that reload restarted the instance. Repeated with no commit pending, and
  the pid read from the instance's lock file, the same daemon survived.
  It sampled on (2753 to 2873 DATA records), logged "cannot reconnect to
  ubus", and published its status again 2 s later. LuCI's `getFeatures`
  answered after rpcd, dnsmasq, odhcpd and uhttpd were restarted.

Left on the VM: the new packages, the instance running native with its
original options, an empty exec file, no log override; the harness files were
removed.

## Addendum: the exec file moves to /etc/cake-adapt/exec

After the user moved the root-only file to `/etc/cake-adapt/exec` and added
`ping_extra_args` to it (irtt's `-o FILE` makes root write a file),
[`exec-move-check.sh`](exec-move-check.sh) ran on the same VM
([`exec-move-steps.txt`](exec-move-steps.txt)):

- **The new file:** installs root-owned with mode 0600. Real uci reads it,
  comment-only, with `uci -c /etc/cake-adapt show exec`.
- **Reading it:** after a reload by hand, its `ping_extra_args` and
  `config_file` reach the runtime file.
- **Refusal:** `ping_extra_args` in `/etc/config/cake-adapt` is refused, the
  instance keeps its pid, nothing is written to the planted `-o` path, and
  `cake-adapt -S main -V` run by hand refuses it as well.
- **Back on native:** the instance returns to native at the end.

Two checks failed:

- **"old exec file gone"** failed because the earlier lifecycle test had
  rewritten `/etc/config/cake-adapt-exec`. apk therefore kept it as a
  modified configuration file. Nothing reads it, and that file was never
  released; it was deleted by hand.
- **"fping got the args"** used `-S 10.0.2.15`, which is not an address of
  the VM, so fping refused to start. Repeated with eth1's own address, fping
  ran as `/usr/bin/fping -S 10.0.3.15 -I eth1 --timestamp --loop …`.

## Addendum: the log directory becomes root-only

Review found that `log_file_path_override`, still on the LuCI page, let a LuCI
writer choose where root writes the log. Log lines are `;`-separated, so in a
directory such as `/etc/uci-defaults/` or `/etc/hotplug.d/` they would run as
shell commands. It moved to `/etc/cake-adapt/exec` with the other three.
[`log-directory-check.sh`](log-directory-check.sh)
([`log-directory-steps.txt`](log-directory-steps.txt)) shows:

- the log is written to the directory set in the exec file;
- the same option in `/etc/config/cake-adapt` is refused, the instance keeps
  its pid, and nothing is written to `/etc/hotplug.d/iface`;
- the service returns to `/var/log` afterwards.

A first attempt also wrapped the exec read in a private delta directory
(`uci -t`). On the VM, real uci still applied an uncommitted
`/tmp/.uci/exec` change: libuci always searches `/tmp/.uci`, and no option
turns that off. That change was dropped. `/tmp/.uci` is root-only (0700), so
only root's own uncommitted changes apply, as for every UCI file.
