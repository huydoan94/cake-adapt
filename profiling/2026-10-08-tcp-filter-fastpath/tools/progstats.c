/* Prints "ID: type name NAME run_time_ns N run_cnt N" per loaded BPF program, like bpftool prog show. */
#include <linux/bpf.h>
#include <stdio.h>
#include <string.h>
#include <sys/syscall.h>
#include <unistd.h>

static long bpf(int cmd, union bpf_attr *attr) { return syscall(__NR_bpf, cmd, attr, sizeof(*attr)); }

int main(void)
{
	union bpf_attr attr;
	__u32 id = 0;

	for (;;) {
		struct bpf_prog_info info;
		int fd;

		memset(&attr, 0, sizeof(attr));
		attr.start_id = id;
		if (bpf(BPF_PROG_GET_NEXT_ID, &attr) != 0)
			break;
		id = attr.next_id;
		memset(&attr, 0, sizeof(attr));
		attr.prog_id = id;
		fd = (int)bpf(BPF_PROG_GET_FD_BY_ID, &attr);
		if (fd < 0)
			continue;
		memset(&info, 0, sizeof(info));
		memset(&attr, 0, sizeof(attr));
		attr.info.bpf_fd = (__u32)fd;
		attr.info.info_len = sizeof(info);
		attr.info.info = (__u64)(unsigned long)&info;
		if (bpf(BPF_OBJ_GET_INFO_BY_FD, &attr) == 0)
			printf("%u: type%u  name %s  run_time_ns %llu run_cnt %llu\n", id, info.type, info.name,
			       (unsigned long long)info.run_time_ns, (unsigned long long)info.run_cnt);
		close(fd);
	}
	return 0;
}
