#include "bpffilter.h"
#include "efilters/bootstrap.h"
#include "bootstrap.skel.h"

#include <thread>

#include <time.h>
#include <bpf/libbpf.h>

#include <QDebug>

static int libbpf_print_fn(enum libbpf_print_level level, const char *format, va_list args)
{
	if (level == LIBBPF_DEBUG)
		return 0;
	return vfprintf(stderr, format, args);
}

static int handle_event(void *ctx, void *data, size_t data_sz)
{
	if (ctx) {
		BpfFilter *f = (BpfFilter *)ctx;
		return f->handleKernelEvent(data, data_sz);
	}

	const struct event *e = (const struct event *)data;
	struct tm *tm;
	char ts[32];
	time_t t;

	time(&t);
	tm = localtime(&t);
	strftime(ts, sizeof(ts), "%H:%M:%S", tm);

	if (e->exit_event) {
		printf("%-8s %-5s %-16s %-7d %-7d [%u]",
			   ts, "EXIT", e->comm, e->pid, e->ppid, e->exit_code);
		if (e->duration_ns)
			printf(" (%llums)", e->duration_ns / 1000000);
		printf("\n");
	} else {
		printf("%-8s %-5s %-16s %-7d %-7d %s\n",
			   ts, "EXEC", e->comm, e->pid, e->ppid, e->filename);
	}

	return 0;
}

class BpfFilterPriv
{
public:
	struct bootstrap_bpf *skel = nullptr;
	struct ring_buffer *rb = nullptr;
	std::thread pollThread;
	bool exiting = false;
};

BpfFilter::BpfFilter()
{
	p = new BpfFilterPriv;
}

BpfFilter::~BpfFilter()
{
	/* Clean up */
	if (p->rb)
		ring_buffer__free(p->rb);
	if (p->skel)
		bootstrap_bpf__destroy(p->skel);
	delete p;
}

int BpfFilter::open()
{
	/* Load and verify BPF application */
	p->skel = bootstrap_bpf__open();
	if (!p->skel)
		return 1;

	/* Parameterize BPF code with minimum duration parameter */
	p->skel->rodata->min_duration_ns = 0 * 1000000ULL;

	/* Load & verify BPF programs */
	int err = bootstrap_bpf__load(p->skel);
	if (err)
		return 2;

	p->rb = ring_buffer__new(bpf_map__fd(p->skel->maps.rb), handle_event, this, NULL);
	if (!p->rb)
		return 3;

	return 0;
}

int BpfFilter::attach()
{
	/* Attach tracepoints */
	int err = bootstrap_bpf__attach(p->skel);
	if (err)
		return 1;

	return 0;
}

int BpfFilter::start()
{
	p->pollThread = std::thread([this]() {
		while (!p->exiting) {
			int err = ring_buffer__poll(p->rb, 100 /* timeout, ms */);
			/* Ctrl-C will cause -EINTR */
			if (err == -EINTR) {
				ring_buffer__free(p->rb);
				bootstrap_bpf__destroy(p->skel);
				p->rb = nullptr;
				p->skel = nullptr;
				break;
			}
			if (err < 0) {
				printf("Error polling perf buffer: %d\n", err);
				break;
			}
		}
	});

	return 0;
}

int BpfFilter::stop()
{
	p->exiting = true;
	p->pollThread.join();
	return 0;
}

int BpfFilter::handleKernelEvent(void *data, size_t size)
{
	const struct event *e = (const struct event *)data;
	if (e->exit_event) {
		return 0;
	}
	qDebug("new process started: %s, filename is %s", e->comm, e->filename);
	return 0;
}

void BpfFilter::initbpf()
{
	/* Set up libbpf errors and debug info callback */
	libbpf_set_print(libbpf_print_fn);
}
