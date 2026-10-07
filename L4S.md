# L4S and cake-adapt

Notes, 2026-10-06. Nothing here is implemented.

## In short

L4S (Low Latency, Low Loss, Scalable throughput) is the IETF's answer to the
same problem cake-adapt attacks: queueing delay. It is a different approach.
Senders mark their packets as L4S-capable, and network queues give them very
early ECN congestion marks instead of drops. Those senders then back off in
small, frequent steps, so their queue stays near 1 ms.

For cake-adapt today:

1. **L4S traffic passes through CAKE safely, without its sub-millisecond
   benefit.** CAKE treats L4S packets as ordinary ECN traffic and marks them by
   its usual 5 ms rule. Per-flow fairness keeps L4S flows from crowding others
   out. Nothing needs to change, and nothing breaks.
2. **Where the ISP runs an L4S queue, shaping below the line rate gives it
   up.** This is the case on Comcast's low-latency DOCSIS. cake-adapt then
   moves the bottleneck into CAKE, which has no L4S queue. That trade is still
   right on a bloated line, and SaskTel's network is not known to run L4S.
3. **ECN marks are a congestion signal cake-adapt could use.** It does not
   read them yet. CE marks on arriving packets, or echoed back in ACKs, report
   a queue beyond the router, with no reflector, no ping and no clock problem.
   This fits the eBPF filter and the plan to replace fping
   ([`EBPF_LATENCY_PLAN.md`](EBPF_LATENCY_PLAN.md)).

The recommended first step is cheap. Count the ECN codepoints the filter
already sees, in the phase 0 census of that plan. That shows how much of your
traffic is L4S, and whether anything upstream marks CE at all.

## L4S in brief

| Part | What it is |
| --- | --- |
| Identifier ([RFC 9331](https://datatracker.ietf.org/doc/html/rfc9331)) | A sender sets ECN codepoint **ECT(1)** (IP header ECN bits `01`). Classic ECN uses ECT(0) (`10`). CE (`11`) is the congestion mark. |
| Scalable congestion control | The sender reduces in proportion to the share of marked packets, like DCTCP, instead of halving on a mark. Examples: TCP Prague (Linux, out of tree), Apple's QUIC and TCP stacks, BBRv2/v3 in L4S mode. |
| DualQ Coupled AQM ([RFC 9332](https://datatracker.ietf.org/doc/html/rfc9332)) | Two queues: ECT(1)/CE traffic in a low-latency queue marked at about 1 ms, and everything else in a classic queue (PI2, about 15 ms target). Coupling keeps the two classes fair to each other. Linux has it as the `dualpi2` qdisc. |
| Architecture ([RFC 9330](https://datatracker.ietf.org/doc/html/rfc9330)) | How the parts fit together, including coexistence with classic ECN queues and with flow-queuing AQMs such as fq_codel and CAKE. |

### Deployment as of 2026

- **Clients:** Apple's systems support L4S from iOS 17, iPadOS 17,
  macOS 14 and tvOS 17. In recent releases its setting defaults to automatic.
  Windows and stock Linux TCP do not send ECT(1).
- **ISPs:** Comcast began rolling out L4S on DOCSIS (Low Latency DOCSIS) in
  2025 and planned to extend it to all Xfinity customers. Nothing published
  says SaskTel does.
- **Linux:** `dualpi2` was merged in Linux 6.17. OpenWrt 25.12 runs 6.12, so
  routers on it do not have it. fq_codel has had an L4S-style option since
  5.16: `ce_threshold` with `ce_threshold_selector 0x01/0x01` marks only
  ECT(1) packets once their queue time passes a shallow threshold. CAKE has no
  equivalent option.

## How CAKE treats L4S traffic

CAKE marks ECN-capable packets, ECT(0) and ECT(1) alike, with CE using its
COBALT AQM (CoDel with a 5 ms target and 100 ms interval). It drops packets
that are not ECN-capable. For an L4S flow this means:

- **Fewer, later marks than L4S expects.** CoDel starts marking only after the
  queue has stayed above 5 ms for an interval, then speeds up slowly. An L4S
  sender reads each mark as a small signal. Its own queue therefore settles
  around CAKE's target rather than at 1 ms.
- **Fairness is still enforced.** In a single shared classic queue, L4S flows
  can take more than their share, because they back off less per mark. RFC 9330
  and RFC 9331 discuss this. CAKE gives each flow its own queue and serves them
  in turn, so an L4S flow cannot crowd out others. L4S senders are also
  required to detect a classic ECN bottleneck and fall back to classic
  behavior (RFC 9331, section 4.3).
- **ECN bits survive.** CAKE's `wash` option clears DSCP, not ECN. SQM's
  ingress `ctinfo` restores DSCP from conntrack, also without touching ECN.

Result: L4S traffic behind CAKE gets CAKE's normal latency, a few ms, instead
of about 1 ms. It loses nothing compared with classic traffic. This is
reasoning from the specifications and CAKE's design, not something measured
here (see [Testing](#testing)).

## Where L4S meets cake-adapt

### 1. Which queue is the bottleneck

cake-adapt exists to keep the bottleneck queue in CAKE, because the ISP's queue
is assumed to be large and unmanaged (bufferbloat). With an L4S ISP queue that
assumption weakens:

| ISP queue | Without cake-adapt | With cake-adapt shaping below the line rate |
| --- | --- | --- |
| Unmanaged FIFO (bloated) | hundreds of ms under load | a few ms: the reason cake-adapt exists |
| Classic AQM | classic flows: AQM target, often 10-20 ms | a few ms, plus per-flow fairness |
| L4S DualQ | L4S flows about 1 ms; classic flows about 15 ms | everything a few ms, plus per-flow fairness; L4S's 1 ms is lost, and some throughput is given up |

So on an L4S line, cake-adapt still helps classic traffic and fairness, but
costs L4S traffic and throughput. A future option could detect a managed ISP
queue, for example from upstream CE marks and low added delay at full rate. It
could then raise the shaper toward the line rate. That would be a deliberate,
opt-in departure, measured like the others. Nothing suggests your line needs
it.

### 2. ECN as a congestion signal (fits the eBPF plan)

The TCP filter sits after the upload CAKE, and before the download CAKE on the
IFB. It sees:

- **Download:** CE on an arriving packet was set upstream of the router, by
  the ISP or beyond, never by our download CAKE. A rising CE rate on download
  is direct evidence of an upstream queue, without any delay measurement.
- **Upload:** CE set on our upload packets by the ISP's upstream queue comes
  back as the ECE flag in incoming TCP ACKs (or as counters with AccECN). Our
  own upload CAKE also marks before the packet leaves, so ECE mixes both. CAKE
  reports its own marks per tin over netlink, so they could be subtracted
  approximately. Classic ECE is a flag, not a count, so this stays a rough
  signal.

Only ECN-capable traffic carries the signal, and only queues that mark rather
than drop produce it. On a line without an ECN-marking AQM it stays silent.
Unmanaged FIFO queues, the bufferbloat case cake-adapt targets, drop instead.
So it complements delay measurement, and cannot replace it on bloated lines.

Concrete steps that fit the existing plan:

1. **Phase 0 census:** per direction, count packets and bytes by ECN codepoint
   (Not-ECT, ECT(0), ECT(1), CE) on arrival, and ECE flags on incoming ACKs.
   That is a few instructions per packet in the filter, beside the existing
   counters.
2. **If CE arrives under load:** log CE rates beside the delay records, and
   compare their onset with fping's delay rise on your router and on the
   testbed.
3. **Only then** consider CE as an extra, opt-in input to the controller.
   That would be a departure from cake-autorate, with its own evidence.

### 3. Future ingress ownership

If cake-adapt ever takes over the ingress path (`ctinfo`, `mirred`, IFB), the
DSCP restore must keep masking out the ECN bits, as it does today. Clearing or
rewriting ECN there would break both classic ECN and L4S for downloads.

### 4. What not to do

- Do not replace CAKE with `dualpi2` to "support L4S". `dualpi2` is not a
  shaper with CAKE's overhead compensation, host fairness and DiffServ. It is
  not in OpenWrt 25.12's kernel, and cake-adapt's scope is controlling CAKE.
- Do not strip ECT(1) or remap it to ECT(0). Behind CAKE it is harmless, and
  past CAKE it may reach an L4S queue that helps it.

## Integrating L4S: does CAKE still matter? (discussion, 2026-10-06)

**Question:** L4S makes each app on a device back off, but every device's
traffic still goes through the gateway. Does CAKE still have a use, for
example bandwidth fairness?

**Summary of the answer:** yes. L4S is a deal between the sender, which marks
ECT(1) and backs off in small steps, and the bottleneck queue, which must mark
those packets at about 1 ms. It does nothing without an L4S-aware queue at the
bottleneck. CAKE remains necessary because:

- **Not L4S:** most traffic doesn't use it (Windows, most Android and Linux
  TCP, games, VPNs, calls), and still needs CAKE's AQM.
- **Fairness:** L4S has no per-host or per-flow fairness. DualPI2 only couples
  the L4S and classic classes to each other.
- **Bloated ISP:** L4S cannot help there, since a dumb FIFO never marks.
  Shaping is what creates a managed queue at home.
- **The rest of CAKE:** overhead compensation, DiffServ and the ACK filter
  have no L4S counterpart.

The ideal would be CAKE plus shallow ECT(1) marking inside it, but CAKE cannot
do that today:

| Option | Cost |
| --- | --- |
| A. Patch `sch_cake` for about 1 ms ECT(1) marking per flow queue, like fq_codel's `ce_threshold_selector` | A kernel module fork for every target. Upstream acceptance is uncertain. It goes beyond "control the existing CAKE". |
| B. fq_codel with `ce_threshold_selector` behind HTB, with cake-adapt controlling HTB | Loses host fairness, overhead handling and DiffServ: a big scope change. |
| C. Keep CAKE | L4S flows get CAKE's few ms instead of about 1 ms. |

At 145/73 Mbit/s a flow's queue in CAKE already stays at a few ms. Its
per-flow queues also keep an L4S flow from waiting behind others, so option A
would save each L4S flow a few ms. That is small next to the bloat cake-adapt
already removes.

**Decision so far:** measure before building.

1. Run the ECN census (phase 0 above) on the router.
2. Only if L4S traffic turns out to be significant, prototype option A on the
   VM with real L4S senders, and compare it with stock CAKE.

## Testing

The emulated testbed could cover:

- **CAKE marks ECT(1) like ECT(0):** send UDP with the ECN field set to
  ECT(1) or ECT(0) (for example `iperf3 -u --tos 0x01` and `--tos 0x02`) into
  an overloaded CAKE. Then compare CE marks in a capture after CAKE, and
  CAKE's own `ecn_mark` counters.
- **Upstream CE seen by the filter:** put an ECN-marking AQM in the
  emulated ISP, fq_codel with `ecn` instead of the plain tbf FIFO. Run ECN TCP
  (`net.ipv4.tcp_ecn=1` on the endpoints) and check that the census counts CE
  on download and ECE on upload ACKs.
- **Real L4S senders:** these need TCP Prague (the
  [L4STeam kernel](https://github.com/L4STeam/linux)) on a separate VM, or Apple
  clients on a real network. That is a larger setup, worth it only if phase 0
  shows meaningful ECT(1) traffic.

## Sources

- [RFC 9330, L4S architecture](https://datatracker.ietf.org/doc/html/rfc9330)
- [RFC 9331, L4S ECN identifier](https://datatracker.ietf.org/doc/html/rfc9331)
- [RFC 9332, DualQ Coupled AQM](https://datatracker.ietf.org/doc/html/rfc9332)
- [L4S overview (Wikipedia)](https://en.wikipedia.org/wiki/L4S)
- [Linux 6.17 release notes (DualPI2)](https://kernelnewbies.org/Linux_6.17)
- [fq_codel: L4S-style ce_threshold_ect1 marking (patch)](https://patchwork.kernel.org/project/netdevbpf/patch/20211014175918.60188-3-eric.dumazet@gmail.com/)
- [tc-fq_codel(8)](https://linuxman7.org/linux/man-pages/man8/tc-fq_codel.8.html)
- [Comcast's L4S rollout (RCR Wireless, 2025-01-29)](https://www.rcrwireless.com/20250129/uncategorized/comcast-l4s)
- [L4S in a partial deployment (arXiv 2411.10952)](https://arxiv.org/html/2411.10952v1)
- [Piece of CAKE (arXiv 1804.07617)](https://arxiv.org/pdf/1804.07617)
- [L4STeam Linux tree (TCP Prague, dualpi2)](https://github.com/L4STeam/linux)
