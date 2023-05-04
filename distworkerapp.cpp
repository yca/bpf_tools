#include "distproxyserver.h"

#include <rpc/client.h>
#include <commonpp/mainappcontext.h>

#include <QUuid>
#include <QDebug>
#include <QProcess>

#include <thread>

using namespace std::chrono_literals;

static WorkerJobResponse runProcess(const WorkerJobRequest &job)
{
	qDebug() << "running" << job.jobRun.program.data();
	WorkerJobResponse jobResp;
	QProcess p;
	p.setProgram(job.jobRun.program.data());
	QStringList qargs;
	for (int i = 0; i < job.jobRun.arguments.size(); i++)
		qargs << job.jobRun.arguments[i].data();
	p.setArguments(qargs);
	p.start();
	p.waitForStarted(-1);
	p.waitForFinished(-1);
	{
		const auto &ba = p.readAllStandardOutput();
		jobResp.jobRun.out.append(QString::fromUtf8(ba).trimmed().toStdString());
	}
	{
		const auto &ba = p.readAllStandardError();
		jobResp.jobRun.err.append(QString::fromUtf8(ba).trimmed().toStdString());
	}
	jobResp.jobRun.exitCode = p.exitCode();

	return jobResp;
}

int workermain(MainAppContext &ctx)
{
	rpc::client c("127.0.0.1", 40001);
	int jobSimDuration = 1000;
	int timeout = 10000;
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
		if (!res.error) {
			CompleteRequest req;
			req.workerid = myuuid;
			qDebug("we have a new job to do, type=%d", res.job.jobType);
			if (res.job.jobType == REMOTE_JOB_RUN_PROCESS)
				req.jobResp = runProcess(res.job);
			else if (res.job.jobType == REMOTE_JOB_SIMULATE)
				std::this_thread::sleep_for(jobSimDuration * 1ms);
			req.jobResp.jobType = res.job.jobType;
			req.jobResp.jobRun.out = "hmmm";
			const auto &res = c.call("complete", req).as<CompleteResponse>();
			if (--jobCount == 0)
				break;
		} else {
			qDebug("job request error %d", res.error);
		}
	}

	return 0;
}
