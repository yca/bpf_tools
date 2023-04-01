#ifndef DISTPROXYSERVER_H
#define DISTPROXYSERVER_H

#include "functionprofiler.h"

#include <rpc/server.h>

#include <mutex>
#include <unordered_set>
#include <condition_variable>

struct DistributeRequest {
	int waitTimeout = 0;			//in msecs
	bool includeProfileData = false;
	MSGPACK_DEFINE_ARRAY(waitTimeout, includeProfileData)
};

struct DistributeResponse {
	int error = 0;
	std::string profileData;
	MSGPACK_DEFINE_ARRAY(error, profileData)
};

struct CompleteRequest {
	std::string workerid;
	std::string jobid;
	MSGPACK_DEFINE_ARRAY(workerid, jobid)
};

struct CompleteResponse {
	int error = 0;
	MSGPACK_DEFINE_ARRAY(error)
};

struct JobRequest {
	int timeout = 0;				//in msecs
	std::string uuid;
	MSGPACK_DEFINE_ARRAY(uuid, timeout)
};

struct JobResponse {
	int error = 0;
	MSGPACK_DEFINE_ARRAY(error)
};

struct RegisterRequest {
	std::string uuid;
	MSGPACK_DEFINE_ARRAY(uuid)
};

struct RegisterListingRequest {
	bool onlyActive = false;
	MSGPACK_DEFINE_ARRAY(onlyActive)
};

struct RegisterListingResponse {
	std::vector<std::string> list;
	MSGPACK_DEFINE_ARRAY(list)
};

struct WorkerObject {
	std::mutex m;
	std::string uuid;
	std::atomic<bool> jobReady{false};
	std::condition_variable cv;

	int waitJobResult(int timeoutms);
	void distributeJob();
	int waitForAssignment(int timeoutms);
	void completeJob();
	bool isBusy();

	/* assigned job fields */
	std::string jobid;
	//bool completed = false;
	std::atomic<bool> resultReady{false};
	std::condition_variable cvw;
};

class DistProxyServer
{
public:
	DistProxyServer(uint16_t port);

	struct ProxyStatistics {
		int64_t totalJobDistributeRequests = 0;
		int64_t successfullyDistributedJobRequests = 0;
		int64_t failedJobDistributionRequests = 0;
		int64_t timedOutJobs = 0;
		int64_t successfullyCompletedJobs = 0;
		int64_t timedOutWorkers = 0;
		int64_t assignedWorkers = 0;
		int64_t spuriousCompletions = 0;
		int64_t successfulCompletions = 0;
	};

	int start();
	ProxyStatistics getProxyStats() { return stats; }
	const FunctionProfiler & getDistributionProfile();

	std::function<int(const DistributeRequest &, WorkerObject &)> jobAssignmentHandler = nullptr;
	std::function<int(const CompleteRequest &, WorkerObject &)> jobCompletionHandler = nullptr;

protected:
	CompleteResponse serveComplete(const CompleteRequest &req);
	JobResponse serveRequest(const JobRequest &req);
	DistributeResponse serveDistribute(const DistributeRequest &req);

protected:
	/**
	 * @brief The WorkerPool class
	 *
	 * This class is a thread-safe worker manager. It handles all the
	 * state transitions of workers in a thread-safe manner
	 */
	class WorkerPool
	{
	public:
		WorkerObject * getWorker(const std::string &workerid);
		WorkerObject * reserveWorker();
		WorkerObject * releaseWorker();
		WorkerObject * getBusyWorker(const std::string &workerid);
		int markWorkerAsFree(WorkerObject *w);
		WorkerObject *markWorkerAsFree(const std::string &workerid);
		int markWorkerAsBusy(WorkerObject *w);
		int markWorkerAsUnknown(WorkerObject *w);
		void registerWorker(const std::string &uuid);
		void unregisterWorker(const std::string &uuid);
		std::vector<std::string> registeredWorkers();

	protected:
		std::mutex lock;
		std::unordered_set<std::string> freeWorkers;
		std::unordered_set<std::string> reservedWorkers;
		std::unordered_set<std::string> busyWorkers;
		std::unordered_map<std::string, WorkerObject> registeredRunners;
	};
	class ReadyWorkerContext
	{
	public:
		ReadyWorkerContext(const std::string &uuid, WorkerPool *pool)
		{
			this->pool = pool;
			w = pool->markWorkerAsFree(uuid);
		}
		~ReadyWorkerContext()
		{
			/* mark as unknown if not busy */
			if (!pool->getBusyWorker(w->uuid))
				pool->markWorkerAsUnknown(w);
		}
		int waitForAssignment(int timeoutms);

		WorkerObject *w = nullptr;
	protected:
		WorkerPool *pool;
	};
	rpc::server srv;
	ProxyStatistics stats;
	WorkerPool wpool;

	FunctionProfiler distProfile;
	FunctionProfiler completeProfile;
};

#endif // DISTPROXYSERVER_H
