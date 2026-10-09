// SPDX-License-Identifier: GPL-2.0-only
/* The floor of a tcx program's measured cost: it only returns. */
#include <linux/bpf.h>
#include <bpf/bpf_helpers.h>
SEC("tcx/egress") int empty_egress(struct __sk_buff *skb) { return TCX_NEXT; }
SEC("tcx/ingress") int empty_ingress(struct __sk_buff *skb) { return TCX_NEXT; }
char LICENSE[] SEC("license") = "GPL";
