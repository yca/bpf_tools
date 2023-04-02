#include "distproxyserver.h"

#include <commonpp/mainappcontext.h>

#include <QTimer>
#include <QCoreApplication>

int distproxymain(MainAppContext &ctx)
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

