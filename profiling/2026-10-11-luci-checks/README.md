# LuCI checks every option; Status from procd (x86 VM)

`luci-app-cake-adapt` 0.3.16-r1 built from `b235b01` and installed over the
earlier build on the OpenWrt 25.12 x86 VM 192.168.56.2 (cake-adapt 0.3.16-r1
left as it was). Checksums of the apk and of the installed view files:
`raw/sha256.txt` (the installed `config.js`, `status.js` and ACL hash the same
as the sources, see `raw/vm-checks.txt`).

## What was checked

- Host: `tests/luci_config.js` (every rule of `config.c` and `validate.c`, one
  passing and one failing case, defaults for empty fields, a drift check that
  the view declares every option the rules mention, and the re-check of all
  fields on a change) and `tests/luci_status.js` (procd replies: running with
  and without status, stopped, disabled, missing service, procd silent):
  `raw/host-luci-tests.txt`.
- VM, over `/ubus` with a temporary rpcd login limited to the app's groups plus
  `luci-base` (removed afterwards, `/etc/config/rpcd` restored,
  `raw/vm-checks.sh`, `raw/vm-checks.txt`):
  - `service list {"name":"cake-adapt"}` is allowed and returns the instance
    with `running: true` and its `pid`; `service set` and `service delete` are
    "Access denied", as is `network.interface dump`.
  - After `/etc/init.d/cake-adapt stop` procd's reply carries no object at all
    (`[0]`), `rc list` says `running: false` and `cake-adapt.main status`
    answers "Object not found"; after `start` procd lists the new pid. The
    Status view reads a procd reply without the service as "Not running".
  - The served `config.js` and `status.js` hash as the sources (HTTP 200).
  - UCI identical before and after, the service running at the end.

## Limits

Not a browser: the JavaScript of the pages, the red fields as one types and the
Save refusal in the browser were not exercised. They rest on the stock LuCI
mechanisms (`validate` on each option, `Map.save` validating every field,
`Option.parse` rejecting an invalid one) and on the stubbed host tests. The look
of both pages needs a person's eyes.

## Per-instance actions (2026-10-11, later the same day)

After the Status page moved Start, Restart and Stop to each instance, the
ACL grants exec of `/etc/init.d/cake-adapt start|stop|restart <section>`
only, the section starting with a UCI name character. Through `/ubus` with a
temporary rpcd login (removed afterwards): an empty, space-only or missing
section, `enable`, other commands and `/bin/sh` are denied; `restart main`
gave a new pid, and `stop main` / `start main` worked
(`raw/ui-actions.txt`, `raw/ui-actions-acl.txt`). The look in a browser is
the owner's check.
