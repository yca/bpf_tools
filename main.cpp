#include "bpffilter.h"
#include "customprocess.h"
#include "efilters/bootstrap.h"

#include <unordered_map>
#include <unordered_set>

#include <rpc/server.h>
#include <rpcproto.h>
#include <proc/readproc.h>

#include <QDebug>
#include <QCoreApplication>

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

class MainContext
{
public:
	static MainContext parse(int argc, char *argv[])
	{
		MainContext ctx;
		ctx.argc = argc;
		ctx.argv = argv;
		ctx.binname = QString::fromUtf8(argv[0]).split('/').last();
		for (int i = 1; i < argc; i++)
			ctx.extraargs << QString::fromUtf8(argv[i]);
		return ctx;
	}
	int getIntArg(const QString &arg) const
	{
		int ind = extraargs.indexOf(arg);
		if (ind < 0)
			return 0ll;
		return extraargs[ind + 1].toLongLong();
	}
	QString getStringArg(const QString &arg) const
	{
		int ind = extraargs.indexOf(arg);
		if (ind < 0)
			return QString();
		return extraargs[ind + 1];
	}
	bool containsArg(const QString &arg) const
	{
		return extraargs.contains(arg);
	}
	QString getCurrentBinName() const
	{
		return binname;
	}
	void addbin(const std::string &desc, std::function<int(MainContext&)> f)
	{
		binaries[desc] = f;
	};
	int runBinary(const std::string &binname)
	{
		if (binaries.count(binname))
			return binaries[binname](*this);
		qDebug("unknown application: %s", binname.data());
		return -ENOENT;
	}

	int argc;
	char **argv;
	QString binname;
	QStringList extraargs;
	std::unordered_map<std::string, std::function<int(MainContext&)>> binaries;
protected:
};

static int distmain(MainContext &ctx)
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

static int bpftest(MainContext &ctx)
{
	QCoreApplication a(ctx.argc, ctx.argv);

	BpfFilter::initbpf();
	BpfFilter f;
	if (f.open())
		throw std::runtime_error("error opening ebpf filter");
	if (f.attach())
		throw std::runtime_error("error attaching ebpf filter");
	if (f.start())
		throw std::runtime_error("error starting ebpf filter");

	return a.exec();
}

static int rpcmain(MainContext &ctx)
{
	rpc::server srv(18080);
	srv.bind("run", [](const std::vector<std::string> &args) {
		RunResult res;
		res.err = -ENOENT;
		if (args.size() < 1)
			return res;
		qDebug() << "running" << args[0].data();
		QProcess p;
		p.setProgram(args[0].data());
		QStringList qargs;
		for (int i = 1; i < args.size(); i++) {
			if (args[i] == "__env__")
				break;
			qargs << args[i].data();
		}
		p.setArguments(qargs);
		p.start();
		p.waitForStarted(-1);
		p.waitForFinished(-1);
		{
			const auto &ba = p.readAllStandardOutput();
			res.out.append(QString::fromUtf8(ba).trimmed().toStdString());
		}
		{
			const auto &ba = p.readAllStandardError();
			res.err.append(QString::fromUtf8(ba).trimmed().toStdString());
		}
		res.exitCode = p.exitCode();
		return res;
	});
	srv.run();
	return 0;
}

int main(int argc, char *argv[])
{
	auto ctx = MainContext::parse(argc, argv);
	ctx.addbin("bpf_test", bpftest);
	ctx.addbin("distribute", distmain);
	ctx.addbin("rpcrunner", rpcmain);

	if (ctx.getCurrentBinName() == "bpf_tools") {
		if (ctx.containsArg("--show-binaries")) {
			for (const auto &key: ctx.binaries)
				qDebug("%s", key.first.data());
			return 0;
		}
		if (ctx.containsArg("--print-linking-commands")) {
			for (const auto &key: ctx.binaries)
				qDebug("ln -s bpf_tools %s", key.first.data());
			return 0;
		}
		qDebug("usage: bpf_tools [--show-binaries] [--print-linking-commands]");
		return 0;
	}

	return ctx.runBinary(ctx.binname.toStdString());
}
