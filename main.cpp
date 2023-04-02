#include "bpffilter.h"
#include "customprocess.h"
#include "distproxyserver.h"
#include "efilters/bootstrap.h"

#include <thread>
#include <unordered_map>
#include <unordered_set>

#include <rpcproto.h>
#include <rpc/server.h>
#include <rpc/client.h>
#include <gtest/gtest.h>
#include <proc/readproc.h>

#include <QUuid>
#include <QDebug>
#include <QTimer>
#include <QElapsedTimer>
#include <QCoreApplication>

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

static int distproxymain(MainContext &ctx)
{
	QCoreApplication a(ctx.argc, ctx.argv);
	DistProxyServer srv(40001);

	srv.jobAssignmentHandler = [](const DistributeRequest &req, WorkerObject &w) {
		w.job = req.job;
		w.jobid = "";
		return 0;
	};

	srv.start();
	QTimer t;
	t.start(5000);
	QElapsedTimer et;
	et.start();
	DistProxyServer::ProxyStatistics lastStats = srv.getProxyStats();
	QObject::connect(&t, &QTimer::timeout, [&]() {
		auto stats = srv.getProxyStats();
		qDebug("=== Overview ===");
		auto elapsed = et.restart();
		auto dps = (stats.successfullyDistributedJobRequests - lastStats.successfullyDistributedJobRequests) * 1000
				/ elapsed;
		auto cps = (stats.successfullyCompletedJobs - lastStats.successfullyCompletedJobs) * 1000
				/ elapsed;
		qDebug("workers free: %d", srv.freeWorkerCount());
		qDebug("dps=%lld cps=%lld", dps, cps);
		qDebug("=== Details ===");
		qDebug("Total job distribution requests: %ld", stats.totalJobDistributeRequests);
		qDebug("Successful job distribution requests: %ld", stats.successfullyDistributedJobRequests);
		qDebug("Failed job distribution requests: %ld", stats.failedJobDistributionRequests);
		qDebug("Jobs timed-out: %ld", stats.timedOutJobs);
		qDebug("Jobs completed: %ld", stats.successfullyCompletedJobs);
		qDebug("Successfully assigned workers: %ld", stats.assignedWorkers);
		qDebug("Timed-out workers: %ld", stats.timedOutWorkers);
		qDebug("Spurious completions: %ld", stats.spuriousCompletions);
		qDebug("Successful completions: %ld", stats.successfulCompletions);

		lastStats = stats;
	});
	return a.exec();
}

static int workermain(MainContext &ctx)
{
	rpc::client c("127.0.0.1", 40001);
	int jobSimDuration = 1000;
	int timeout = 1000;
	int jobCount = 0;
	if (ctx.containsArg("--job-count"))
		jobCount = ctx.getIntArg("--job-count");
	if (ctx.containsArg("--wait-timeout"))
		timeout = ctx.getIntArg("--wait-timeout");
	if (ctx.containsArg("--sim-duration"))
		jobSimDuration = ctx.getIntArg("--sim-duration");
	auto myuuid = QUuid::createUuid().toString(QUuid::Id128).toStdString();
	{
		RegisterRequest req;
		req.uuid = myuuid;
		if (c.call("register", req).as<int>()) {
			qDebug("error registering ourselves to the proxy");
			return -EINVAL;
		}
	}
	while (1) {
		JobRequest req;
		req.timeout = timeout;
		req.uuid = myuuid;
		const auto &res = c.call("request", req).as<JobResponse>();
		if (!res.error){
			CompleteRequest req;
			req.jobResp.jobType = res.job.jobType;
			qDebug("we have a new job to do, type=%d", res.job.jobType);
			if (res.job.jobType == REMOTE_JOB_RUN_PROCESS) {
				qDebug() << "running" << res.job.jobRun.program.data();
				QProcess p;
				p.setProgram(res.job.jobRun.program.data());
				QStringList qargs;
				for (int i = 0; i < res.job.jobRun.arguments.size(); i++)
					qargs << res.job.jobRun.arguments[i].data();
				p.setArguments(qargs);
				p.start();
				p.waitForStarted(-1);
				p.waitForFinished(-1);
				{
					const auto &ba = p.readAllStandardOutput();
					req.jobResp.jobRun.out.append(QString::fromUtf8(ba).trimmed().toStdString());
				}
				{
					const auto &ba = p.readAllStandardError();
					req.jobResp.jobRun.err.append(QString::fromUtf8(ba).trimmed().toStdString());
				}

				req.jobResp.jobRun.exitCode = p.exitCode();
			}
			req.workerid = myuuid;
			if (jobSimDuration)
				std::this_thread::sleep_for(jobSimDuration * 1ms);
			const auto &res = c.call("complete", req).as<CompleteResponse>();
			if (--jobCount == 0)
				break;
		} else {
			qDebug("job request error %d", res.error);
			//throw std::runtime_error("no job");
		}
	}

	return 0;
}

static int customermain(MainContext &ctx)
{
	rpc::client c("127.0.0.1", 40001);
	int timeout = 1000;
	if (ctx.containsArg("--wait-timeout"))
		timeout = ctx.getIntArg("--wait-timeout");
	int jobCount = 0;
	if (ctx.containsArg("--job-count"))
		jobCount = ctx.getIntArg("--job-count");
	while (1) {
		DistributeRequest req;
		req.job.jobType = REMOTE_JOB_RUN_PROCESS;
		req.job.jobRun.program = "find";
		req.job.jobRun.arguments.push_back(".");
		req.job.jobRun.arguments.push_back("-iname");
		req.job.jobRun.arguments.push_back("\"*.o\"");
		req.includeProfileData = false;
		req.waitTimeout = timeout;
		const auto &res = c.call("distribute", req).as<DistributeResponse>();
		qDebug("request completed with %d", res.error);
		if (res.profileData.size()) {
			qDebug("=== dist profile ===");
			FunctionProfiler profile;
			profile.deserializeFromString(res.profileData);
			const auto &profstats = profile.get();
			for (const auto &[key, value]: profstats) {
				qDebug("section '%s': call=%ld max=%ld avg=%ld", key.data(), value.callCount,
					   value.maxElapsed, value.totalElapsed / value.callCount);
			}
		}
		if (--jobCount == 0)
			break;
	}

	return 0;
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

static int gtestmain(MainContext &ctx)
{
	::testing::InitGoogleTest(&ctx.argc, ctx.argv);
	return RUN_ALL_TESTS();
}

static int wctest()
{
	std::mutex m;
	std::atomic<bool> dataReady{false};
	std::condition_variable cv;
	std::atomic<bool> respReady{false};
	std::condition_variable cv2;

	QElapsedTimer et;
	et.start();
	int64_t cnt = 0;

	auto thr1 = std::thread([&]() {
		while (1) {
			{
				std::unique_lock<std::mutex> lk(m);
				auto status = cv.wait_for(lk, std::chrono::seconds(100), [&](){
					return dataReady.load();
				});
				dataReady = false;
				cnt++;
			}
			respReady = true;
			cv2.notify_all();
		}
	});
	auto thr2 = std::thread([&]() {
		while (1) {
			dataReady = true;
			cv.notify_all();
			std::unique_lock<std::mutex> lk(m);
			cv2.wait_for(lk, std::chrono::seconds(100), [&](){
				return respReady.load();
			});
			respReady = true;
		}
	});
	auto thr3 = std::thread([&]() {
		int64_t last = 0;
		while (1) {
			std::this_thread::sleep_for(std::chrono::seconds(1));
			auto elapsed = et.restart();
			qDebug("ops: %ld", (cnt - last) * 1000 / 1000);
			last = cnt;
		}
	});
	thr1.join();
	thr2.join();
	thr3.join();
	return 0;
}

int main(int argc, char *argv[])
{
	auto ctx = MainContext::parse(argc, argv);
	ctx.addbin("bpf_test", bpftest);
	ctx.addbin("distribute", distmain);
	ctx.addbin("rpcrunner", rpcmain);
	ctx.addbin("distproxy", distproxymain);
	ctx.addbin("distworker", workermain);
	ctx.addbin("customer", customermain);
	ctx.addbin("bpf_tools_testing", gtestmain);

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
		if (ctx.containsArg("--cv"))
			return wctest();
		qDebug("usage: bpf_tools [--show-binaries] [--print-linking-commands]");
		return 0;
	}

	return ctx.runBinary(ctx.binname.toStdString());
}
