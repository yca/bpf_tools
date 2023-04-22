#include "distproxyserver.h"

#include <rpc/client.h>
#include <commonpp/mainappcontext.h>

#include <QDebug>

#include <thread>

using namespace std::chrono_literals;

int customermain(MainAppContext &ctx)
{
	rpc::client c("127.0.0.1", 40001);
	int timeout = 10000;
	if (ctx.containsArg("--wait-timeout"))
		timeout = ctx.getIntArg("--wait-timeout");
	int jobCount = 0;
	if (ctx.containsArg("--job-count"))
		jobCount = ctx.getIntArg("--job-count");
	std::string program;
	std::vector<std::string> programArgs;
	if (ctx.containsArg("--program"))
		program = ctx.getStringArg("--program").toStdString();
	else if (ctx.containsArg("--cmd")) {
		const auto args = ctx.getStringArg("--cmd").split(' ');
		if (args.size())
			program = args.first().toStdString();
		for (int i = 1; i < args.size(); i++)
			programArgs.push_back(args[i].toStdString());
	}
	while (1) {
		DistributeRequest req;
		if (program.size()) {
			req.job.jobType = REMOTE_JOB_RUN_PROCESS;
			req.job.jobRun.program = program;
			req.job.jobRun.arguments = programArgs;
		} else
			req.job.jobType = REMOTE_JOB_SIMULATE;
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
		if (!res.error) {
			qDebug() << res.jobResp.jobType;
			if (res.jobResp.jobType == REMOTE_JOB_RUN_PROCESS) {
				qDebug() << "process exited with code" << res.jobResp.jobRun.exitCode;
				qDebug() << res.jobResp.jobRun.err.data();
				qDebug() << res.jobResp.jobRun.out.data();
			}
		}
		if (--jobCount == 0)
			break;
	}

	return 0;
}
