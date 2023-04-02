#include "bpffilter.h"
#include "customprocess.h"
#include "efilters/bootstrap.h"

#include <rpc/client.h>
#include <commonpp/mainappcontext.h>

#include <QUuid>
#include <QDebug>
#include <QProcess>

#include <thread>

#include <proc/readproc.h>

using namespace std::chrono_literals;

class BuildBpfFilter : public BpfFilter
{
public:
	struct ProcessInfo {
		pid_t pid;
		std::string program;
		std::string cmdline;
	};
	const ProcessInfo * getProcessEntry(pid_t pid)
	{
		auto getParentPID = [](pid_t pid){
			proc_t proc_info;
			memset(&proc_info, 0, sizeof(proc_info));
			pid_t pidlist[] = {pid, 0};
			PROCTAB *pt_ptr = openproc(PROC_FILLSTATUS | PROC_PID, pidlist);
			ProcessInfo pinfo;
			pinfo.pid = 0;
			if (readproc(pt_ptr, &proc_info) != 0) {
				pinfo.pid = proc_info.ppid;
				pinfo.program = proc_info.cmd;
			} else {
				qDebug() << "not found";
			}
			closeproc(pt_ptr);
			return pinfo;
		};
		if (!pcache.count(pid))
			pcache[pid] = getParentPID(pid);
		return &pcache[pid];
	}
	void cacheEventAsProcessEntry(const struct event *e)
	{
		if (pcache.count(e->pid))
			return;
		ProcessInfo pi;
		pi.pid = e->pid;
		pi.program = e->filename;
		pi.cmdline = e->comm;
		pcache[e->pid] = pi;
	}
	int handleKernelEvent(void *data, size_t size) override
	{
		const struct event *e = (const struct event *)data;
		if (e->exit_event) {
			return 0;
		}
		//cacheEventAsProcessEntry(e);
		qDebug("new process started: %s, filename is %s", e->comm, e->filename);
		getProcessEntry(e->pid); /* make sure we have this in cache */
		std::vector<int64_t> ptree;
		ptree.push_back(e->pid);
		if (e->pid != rootProcess) {
			ptree.push_back(e->ppid);
			if (e->ppid != rootProcess) {
				const ProcessInfo *pinfo = getProcessEntry(e->ppid);
				while (pinfo->pid > 0) {
					ptree.push_back(pinfo->pid);
					if (pinfo->pid == rootProcess)
						break;
					pinfo = getProcessEntry(pinfo->pid);
				}
			}
		}
		qDebug() << "found" << ptree.size() << "parents";
		for (auto pid: ptree)
			qDebug() << pid << pcache[pid].program.data();
		qDebug() << "mine" << getpid();
		qDebug() << "root" << rootProcess;
		qDebug() << "my parent" << getProcessEntry(getpid())->pid;

		return 0;
	}

	void setRootProcess(int64_t pid)
	{
		rootProcess = pid;
	}

	std::unordered_map<int64_t, ProcessInfo> pcache;

protected:
	int64_t rootProcess = 0;
};

int distmain(MainAppContext &ctx)
{
	BuildBpfFilter f;
	if (f.open())
		throw std::runtime_error("error opening ebpf filter");
	if (f.attach())
		throw std::runtime_error("error attaching ebpf filter");
	if (f.start())
		throw std::runtime_error("error starting ebpf filter");

	CustomProcess p;
	//p.setProcessChannelMode(QProcess::ForwardedChannels);
	QString program = ctx.argv[1];
	QStringList args;
	for (int i = 2; i < ctx.argc; i++) {
		args << ctx.argv[i];
	}
	p.start(program, args);
	p.waitForStarted(-1);
	f.setRootProcess(p.processId());
	p.waitForFinished(-1);

	return 0;
}
