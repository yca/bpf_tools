#include "distproxyserver.h"

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
		std::unique_lock<std::mutex> lk(globalLock);
		registeredRunners[req.uuid].uuid = req.uuid;
		return 0;
	});
	srv.bind("unregister", [this](const RegisterRequest &req) {
		std::unique_lock<std::mutex> lk(globalLock);
		registeredRunners.erase(req.uuid);
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

CompleteResponse DistProxyServer::serveComplete(const CompleteRequest &req)
{
	CompleteResponse resp;
	WorkerObject *w;
	{
		std::unique_lock<std::mutex> lk(globalLock);
		if (!busyWorkers.count(req.workerid)) {
			resp.error = -ENOENT;
			return resp;
		}
		w = &registeredRunners[req.workerid];
		resp.error = 0;
		/* from now on this worker is ready-to-do new things */
		busyWorkers.erase(req.workerid);
	}

	/* let's notify the customer */
	w->cvw.notify_all();

	/* we're done with all */
	return resp;
}

JobResponse DistProxyServer::serveRequest(const JobRequest &req)
{
	JobResponse resp;

	/* put this worker to the queue of free workers and init worker object */
	WorkerObject *w;
	{
		std::unique_lock<std::mutex> lk(globalLock);
		freeWorkers.insert(req.uuid);
		w = &registeredRunners[req.uuid];
		w->uuid = req.uuid;
	}

	/* let's wait on a new job assignment */
	std::unique_lock<std::mutex> lk(w->m);
	int timeout = req.timeout ? req.timeout : 10000;
	auto status = w->cv.wait_for(lk, timeout * 1ms);
	if (status == std::cv_status::timeout) {
		/* no job assigned, notify worker */
		std::unique_lock<std::mutex> lk(globalLock);
		freeWorkers.erase(req.uuid);
		resp.error = -ETIMEDOUT;
		return resp;
	}

	/* we have a new job to do */
	resp.error = 0;
	return resp;
}

DistributeResponse DistProxyServer::serveDistribute(const DistributeRequest &req)
{
	WorkerObject *w = nullptr;

	/* find a free worker */
	DistributeResponse resp;
	{
		std::unique_lock<std::mutex> lk(globalLock);
		if (!freeWorkers.size()) {
			resp.error = -ENOENT;
			return resp;
		}
		w = &registeredRunners[*freeWorkers.begin()];
		freeWorkers.erase(w->uuid);
		busyWorkers.insert(w->uuid);
		/* lock worker so that it cannot dissapear */
		w->m.lock();
	}

	/* now set job details */
	{
		w->jobid = "do smt";
	}

	/* wake-up worker for processing, but don't forget to release its lock */
	w->m.unlock();
	w->cv.notify_all();

	/* we distributed job to the worker, now we wait its response */
	std::unique_lock<std::mutex> lk(w->mw);
	int timeout = req.waitTimeout ? req.waitTimeout : 10000;
	auto status = w->cvw.wait_for(lk, timeout * 1ms);
	if (status == std::cv_status::timeout) {
		/* job not completed in time */
		resp.error = -ETIMEDOUT;
	} else
		resp.error = 0;
	return resp;
}
