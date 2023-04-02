#include "distproxyserver.h"

#include <rpc/client.h>
#include <commonpp/mainappcontext.h>

#include <QDebug>

#include <thread>

using namespace std::chrono_literals;

int customermain(MainAppContext &ctx)
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
