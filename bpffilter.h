#ifndef BPFFILTER_H
#define BPFFILTER_H

#include <stddef.h>

class BpfFilterPriv;

class BpfFilter
{
public:
	BpfFilter();
	virtual ~BpfFilter();

	int open();
	int attach();
	int start();
	int stop();

	virtual int handleKernelEvent(void *data, size_t size);

	static void initbpf();

protected:
	BpfFilterPriv *p;
};

#endif // BPFFILTER_H
