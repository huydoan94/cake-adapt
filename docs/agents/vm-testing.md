# Agent guide: VM testing

Read before touching a test VM. Part of [AGENTS.md](../../AGENTS.md); the test machines and permissions stay in AGENTS.md.

For long VM jobs, run the local bounded `ssh` command through
`sh tools/run-bg.sh build/<name>.log <ssh-command>` and observe completion with
`sh tools/wait-for.sh build/<name>.log` per [COLLABORATION.md](../../COLLABORATION.md),
retaining the VM-side deadline and cleanup traps.

The authorized OpenWrt VM test log is always:

```text
/tmp/sqm-mon-test.log
```

At the beginning of a test:

1. stop every process writing that file;
2. create it once with `touch` if it does not exist, then record its inode;
3. truncate it in place with `: > /tmp/sqm-mon-test.log`;
4. create an isolated log directory and hard-link its expected
   `cake-adapt[.<section>].log` name to that file; and
5. point `log_file_path_override` at the isolated directory.

Never delete, move, replace, or pipe through `tee` to this file. Do not start or
stop the user's `tail -f`. At the end, confirm the inode is unchanged.

The test VMs are for testing only. Their installed packages, service
configuration, filter object and files may be changed freely and need not be
restored; record what was changed (package version, UCI, files, qdisc
handles, options and rates) in the run's evidence so the user can look back.
Never run two controllers on one CAKE, whatever is installed.

Bound every transfer and network operation. Prefer a controlled local endpoint
when public upload/download services are unreliable, verify that `ip route get`
selects the intended interface, and confirm the receiver actually got the test
payload. Exercise sustained download, upload, and bidirectional load; inspect
`LOAD`, `DATA`, and `SHAPER` records; verify rate bounds and recovery; and test
qdisc disappearance/reappearance only after capturing enough state to recreate
the exact original qdisc.

After the test, stop test processes and remove only test-created payloads.
Confirm the test-log inode is unchanged and no test-owned daemon or `fping`
child remains. After killing or stopping `ubusd`, restart `rpcd`, `dnsmasq`,
`odhcpd` and `uhttpd` (or reboot) and check LuCI's `getFeatures` through
`uhttpd` (`curl http://127.0.0.1/ubus`); otherwise LuCI stays broken.

Use exact executable paths or `pidof` for process checks because
`pgrep -af cake-adapt` can match the audit command itself. Do not kill an
unrelated legacy process merely because it owns an `fping` child.

