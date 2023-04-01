#include "distproxyserver.h"

#include <QDebug>

#include <thread>

using namespace std::chrono_literals;

/**
 * Life-cycle of a job worker:
 *
 * Worker nodes register themselves with the "register" and/or "unregister" RPCs.
 * A registered worker means it may be available in the future for computing.
 * Registration is not enought to guarantee of availibility of that worker though.
 *
 * After registration a worker may be in 3 different states:
 *
 *	- UNKNOWN
 *	- FREE
 *	- BUSY
 *
 * Unknown means that wherebouts of the worker is unknown. We know it registered itself,
 * but at the moment it is connected to proxy server so we don't mark it as 'free'.
 * "request" RPC transitions worker to 'free' state. Worker will be in 'free' state for a
 * predefined period at maximum, if we're unable to assign a task to it, we will return
 * with a timeout from "request" RPC and it may later decide to re-queue itself again with
 * the "request" RPC. It will be again in 'UNKNOWN' state for us. Wait time can be
 * configurable on the worker client end with a parameter to the "request" RPC.
 * If any job arrives during this 'free' period for this worker, we notify it with this
 * information and its state is switched to 'busy'. If a worker is not registered prior to
 * "request", then it will be automatically registered during "request".
 *
 * Workers notify the completion of a distributed task with "complete" RPC. "complete" RPC
 * transitions worker state to 'UNKNOWN' and notifies the customer with the job completion
 * and related results.
 *
 * Job distribution for the customers:
 *
 * Customers request a new job with the "distribute" RPC call. If there is not any free
 * workers during this time, we return with error from this RPC call. Otherwise, "distribute"
 * RPC finds a worker, switches it to busy, sets job details to the worker and wakes-up
 * the worker.
 */

DistProxyServer::DistProxyServer(uint16_t port)
	: srv(port)
{
	srv.bind("register", [this](const RegisterRequest &req) {
		wpool.registerWorker(req.uuid);
		return 0;
	});
	srv.bind("unregister", [this](const RegisterRequest &req) {
		wpool.unregisterWorker(req.uuid);
		return 0;
	});
	srv.bind("get_registered", [this](const RegisterListingRequest &req) {
		RegisterListingResponse resp;
		resp.list = wpool.registeredWorkers();
		return resp;
	});
	srv.bind("complete", [this](const CompleteRequest &req) {
		return serveComplete(req);
	});
	srv.bind("request", [this](const JobRequest &req) {
		return serveRequest(req);
	});
	srv.bind("distribute", [this](const DistributeRequest &req) {
		return serveDistribute(req);
	});
}

int DistProxyServer::start()
{
	srv.async_run(1024);
	return 0;
}

const FunctionProfiler & DistProxyServer::getDistributionProfile()
{
	return distProfile;
}

int DistProxyServer::freeWorkerCount()
{
	return wpool.freeWorkerCount();
}

CompleteResponse DistProxyServer::serveComplete(const CompleteRequest &req)
{
	CompleteResponse resp;
	resp.error = 0;
	WorkerObject *w = wpool.getBusyWorker(req.workerid);
	if (!w) {
		stats.spuriousCompletions++;
		resp.error = -ENOENT;
		return resp;
	}

	/* report job finish results */
	if (jobCompletionHandler)
		jobCompletionHandler(req, *w);
	w->completeJob();

	wpool.markWorkerAsUnknown(w);
	stats.successfulCompletions++;
	/* we're done with all */
	return resp;
}

JobResponse DistProxyServer::serveRequest(const JobRequest &req)
{
	JobResponse resp;

	/* mark this worker as free */
	ReadyWorkerContext wctx(req.uuid, &wpool);
	if (!wctx.w) {
		resp.error = -ENOENT;
		return resp;
	}

	/* let's wait on a new job assignment */
	int err = wctx.waitForAssignment(req.timeout);
	if (err) {
		resp.error = err;
		stats.timedOutWorkers++;
		return resp;
	}

	/* we have a new job to do, job details are already done in distribute RPC */
	stats.assignedWorkers++;
	resp.error = 0;
	return resp;
}

DistributeResponse DistProxyServer::serveDistribute(const DistributeRequest &req)
{
	WorkerObject *w = nullptr;
	distProfile.restart();
	DistributeResponse resp;

	/* find a free worker */
	distProfile.startSection("find worker");
	w = wpool.reserveWorker();
	distProfile.endSection();

	/* update our stats and check returned worker */
	stats.totalJobDistributeRequests++;
	if (!w) {
		wpool.releaseWorker();
		stats.failedJobDistributionRequests++;
		resp.error = -ENOENT;
		return resp;
	}
	stats.successfullyDistributedJobRequests++;

	/* now set job details */
	distProfile.startSection("worker job assignment");
	if (jobAssignmentHandler)
		jobAssignmentHandler(req, *w);
	w->distributeJob();
	distProfile.endSection();

	/* we distributed job to the worker, now we wait its response */
	distProfile.startSection("waiting for job completion");
	int err = w->waitJobResult(req.waitTimeout);
	if (err == -ETIMEDOUT) {
		stats.timedOutJobs++;
		/* job not completed in time */
		resp.error = -ETIMEDOUT;
	} else {
		stats.successfullyCompletedJobs++;
		resp.error = 0;
	}
	distProfile.endSection();

	if (req.includeProfileData)
		resp.profileData = distProfile.serializeToString();

	return resp;
}

WorkerObject *DistProxyServer::WorkerPool::getWorker(const std::string &workerid)
{
	std::unique_lock<std::mutex> lk(lock);
	if (registeredRunners.count(workerid))
		return &registeredRunners[workerid];
	return nullptr;
}

WorkerObject *DistProxyServer::WorkerPool::reserveWorker()
{
	std::unique_lock<std::mutex> lk(lock);
	if (!freeWorkers.size())
		return nullptr;
	auto *w = &registeredRunners[*freeWorkers.begin()];
	freeWorkers.erase(w->uuid);
	reservedWorkers.insert(w->uuid);

	return w;
}

WorkerObject *DistProxyServer::WorkerPool::releaseWorker()
{
	std::unique_lock<std::mutex> lk(lock);
	if (!freeWorkers.size())
		return nullptr;
	auto *w = &registeredRunners[*freeWorkers.begin()];
	reservedWorkers.erase(w->uuid);
	freeWorkers.insert(w->uuid);

	return w;
}

WorkerObject *DistProxyServer::WorkerPool::getBusyWorker(const std::string &workerid)
{
	std::unique_lock<std::mutex> lk(lock);
	if (!busyWorkers.count(workerid))
		return nullptr;
	auto *w = &registeredRunners[workerid];
	return w;
}

int DistProxyServer::WorkerPool::markWorkerAsFree(WorkerObject *w)
{
	std::unique_lock<std::mutex> lk(lock);
	busyWorkers.erase(w->uuid);
	freeWorkers.insert(w->uuid);
	reservedWorkers.erase(w->uuid);
	return 0;
}

WorkerObject * DistProxyServer::WorkerPool::markWorkerAsFree(const std::string &workerid)
{
	std::unique_lock<std::mutex> lk(lock);
	if (!registeredRunners.count(workerid))
		return nullptr;
	auto *w = &registeredRunners[workerid];
	w->jobReady = false;
	w->resultReady = false;
	busyWorkers.erase(w->uuid);
	freeWorkers.insert(w->uuid);
	reservedWorkers.erase(w->uuid);
	return w;
}

int DistProxyServer::WorkerPool::markWorkerAsBusy(WorkerObject *w)
{
	std::unique_lock<std::mutex> lk(lock);
	busyWorkers.insert(w->uuid);
	freeWorkers.erase(w->uuid);
	reservedWorkers.erase(w->uuid);
	return 0;
}

int DistProxyServer::WorkerPool::markWorkerAsUnknown(WorkerObject *w)
{
	std::unique_lock<std::mutex> lk(lock);
	freeWorkers.erase(w->uuid);
	busyWorkers.erase(w->uuid);
	reservedWorkers.erase(w->uuid);
	return 0;
}

void DistProxyServer::WorkerPool::registerWorker(const std::string &uuid)
{
	std::unique_lock<std::mutex> lk(lock);
	registeredRunners[uuid].uuid = uuid;
}

void DistProxyServer::WorkerPool::unregisterWorker(const std::string &uuid)
{
	std::unique_lock<std::mutex> lk(lock);
	registeredRunners.erase(uuid);
}

std::vector<std::string> DistProxyServer::WorkerPool::registeredWorkers()
{
	std::unique_lock<std::mutex> lk(lock);
	std::vector<std::string> v;
	for (const auto &e: registeredRunners)
		v.push_back(e.first);
	return v;
}

int DistProxyServer::WorkerPool::freeWorkerCount()
{
	std::unique_lock<std::mutex> lk(lock);
	return freeWorkers.size();
}

int WorkerObject::waitJobResult(int timeoutms)
{
	int timeout = timeoutms ? timeoutms : 10000;
	QElapsedTimer t;
	t.start();
	while (!jobReady.load()) {
		std::this_thread::sleep_for(1ms);
		if (t.elapsed() > timeout)
			return -ETIMEDOUT;
	}

	return 0;
}

void WorkerObject::distributeJob()
{
	jobReady = true;
}

int WorkerObject::waitForAssignment(int timeoutms)
{
	/* let's wait on a new job assignment */
	int timeout = timeoutms ? timeoutms : 10000;
	QElapsedTimer t;
	t.start();
	while (!jobReady.load()) {
		std::this_thread::sleep_for(1ms);
		if (t.elapsed() > timeout)
			return -ETIMEDOUT;
	}

	return 0;
}

void WorkerObject::completeJob()
{
	resultReady = true;
}

bool WorkerObject::isBusy()
{
	std::unique_lock<std::mutex> lk(m);
	return !jobid.empty();
}

int DistProxyServer::ReadyWorkerContext::waitForAssignment(int timeoutms)
{
	int err = w->waitForAssignment(timeoutms);
	if (err)
		return err;
	pool->markWorkerAsBusy(w);
	return 0;
}
