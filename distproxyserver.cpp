#include "distproxyserver.h"

#include <QDebug>

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

const FunctionProfiler &DistProxyServer::getDistributionProfile()
{
	return distProfile;
}

CompleteResponse DistProxyServer::serveComplete(const CompleteRequest &req)
{
	CompleteResponse resp;
	resp.error = 0;
	WorkerObject *w = wpool.transitionWorkerFromBusyToFree(req.workerid);
	if (!w) {
		stats.spuriousCompletions++;
		resp.error = -ENOENT;
		return resp;
	}

	/* report job finish results */
	if (jobCompletionHandler)
		jobCompletionHandler(req, *w);
	w->m.lock();
	w->completed = true;
	w->m.unlock();

	/* let's notify the customer */
	w->cvw.notify_all();

	stats.successfulCompletions++;
	/* we're done with all */
	return resp;
}

JobResponse DistProxyServer::serveRequest(const JobRequest &req)
{
	JobResponse resp;

	/* put this worker to the queue of free workers and init worker object */
	WorkerObject *w = wpool.transitionWorkerFromUnknownToFree(req.uuid);
	if (!w) {
		resp.error = -EBUSY;
		return resp;
	}
	/* workers may call 'request' prior to registering themselves */
	if (w->uuid.empty())
		w->uuid = req.uuid;

	/* let's wait on a new job assignment */
	int err = wpool.waitForAssignment(req.uuid, req.timeout);

	if (err == -ETIMEDOUT) {
		/* no job assigned, notify worker */
		wpool.transitionWorkerToUnknown(req.uuid);
		resp.error = -ETIMEDOUT;
		stats.timedOutWorkers++;
		return resp;
	}

	/* we have a new job to do, job details are done in distribute RPC */
	stats.assignedWorkers++;
	resp.error = 0;
	return resp;
}

DistributeResponse DistProxyServer::serveDistribute(const DistributeRequest &req)
{
	WorkerObject *w = nullptr;

	distProfile.restart();

	/* find a free worker */
	DistributeResponse resp;
	/*
	 * locked API ensures that returned worker
	 * will not dissappear, we will need to unlock
	 * it when we're done
	 */
	distProfile.startSection("find worker");
	w = wpool.getFreeWorker();
	distProfile.endSection();

	/* update our stats and check returned worker */
	stats.totalJobDistributeRequests++;
	if (!w) {
		stats.failedJobDistributionRequests++;
		resp.error = -ENOENT;
		return resp;
	}
	stats.successfullyDistributedJobRequests++;

	w->m.lock();
	/* now set job details */
	distProfile.startSection("worker job assignment");
	if (jobAssignmentHandler)
		jobAssignmentHandler(req, *w);
	w->completed = false;
	distProfile.endSection();
	w->m.unlock();

	/* wake-up worker for processing, but remember no to call it in locked state */
	distProfile.startSection("waking-up worker");
	w->cv.notify_all();
	distProfile.endSection();

	/* we distributed job to the worker, now we wait its response */
	distProfile.startSection("waiting for job completion");
	auto err = wpool.waitForCompletion(w->uuid, req.waitTimeout);
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

WorkerObject * DistProxyServer::WorkerPool::getFreeWorker()
{
	std::unique_lock<std::mutex> lk(lock);
	if (!freeWorkers.size())
		return nullptr;
	auto *w = &registeredRunners[*freeWorkers.begin()];
	freeWorkers.erase(w->uuid);
	busyWorkers.insert(w->uuid);

	return w;
}

/**
 * @brief DistProxyServer::WorkerPool::transitionWorkerToFree
 * @param workerid
 * @return
 *
 * This function performs transition if only the worker is in unknown state,
 * this function returns nullptr in case worker is still busy.
 */
WorkerObject *DistProxyServer::WorkerPool::transitionWorkerFromUnknownToFree(const std::string &workerid)
{
	std::unique_lock<std::mutex> lk(lock);
	if (busyWorkers.count(workerid))
		return nullptr;
	auto *w = &registeredRunners[workerid];
	/* from now on this worker is ready-to-do new things */
	freeWorkers.insert(workerid);
	return w;
}

WorkerObject *DistProxyServer::WorkerPool::transitionWorkerToUnknown(const std::string &workerid)
{
	std::unique_lock<std::mutex> lk(lock);
	if (freeWorkers.count(workerid))
		return nullptr;
	auto *w = &registeredRunners[workerid];
	freeWorkers.erase(workerid);
	return w;
}

WorkerObject *DistProxyServer::WorkerPool::transitionWorkerToUnknownLocked(const std::string &workerid)
{
	auto *w = transitionWorkerToUnknown(workerid);
	w->m.lock();
	return w;
}

WorkerObject *DistProxyServer::WorkerPool::transitionWorkerFromBusyToFree(const std::string &workerid)
{
	std::unique_lock<std::mutex> lk(lock);
	if (!busyWorkers.count(workerid))
		return nullptr;
	auto *w = &registeredRunners[workerid];
	/* from now on this worker is ready-to-do new things */
	busyWorkers.erase(workerid);
	return w;
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

int DistProxyServer::WorkerPool::waitForAssignment(const std::string &workerid, int timeoutms)
{
	auto *w = &registeredRunners[workerid];
	/* let's wait on a new job assignment */
	std::unique_lock<std::mutex> lk(w->m);
	int timeout = timeoutms ? timeoutms : 10000;
	auto status = w->cv.wait_for(lk, timeout * 1ms);
	if (status == std::cv_status::timeout)
		return -ETIMEDOUT;

	return 0;
}

int DistProxyServer::WorkerPool::waitForCompletion(const std::string &workerid, int timeoutms)
{
	auto *w = &registeredRunners[workerid];
	/* we distributed job to the worker, now we wait its response */
	int timeout = timeoutms ? timeoutms : 10000;
	std::unique_lock<std::mutex> lk(w->m);
	if (!w->completed) {
		auto status = w->cvw.wait_for(lk, timeout * 1ms);
		if (status == std::cv_status::timeout)
			return -ETIMEDOUT;
	}

	return 0;
}
