#ifndef DISTPROXYSERVER_H
#define DISTPROXYSERVER_H

#include "functionprofiler.h"

#include <rpc/server.h>

#include <mutex>
#include <unordered_set>
#include <condition_variable>

struct RunProgramJobRequest {
	std::string program;
	std::vector<std::string> arguments;
	MSGPACK_DEFINE_ARRAY(program, arguments)
};

struct RunProgramJobResponse {
	std::string out;
	std::string err;
	int exitCode = 0;
	MSGPACK_DEFINE_ARRAY(out, err, exitCode)
};

enum RemoteJobType {
	REMOTE_JOB_RUN_PROCESS,
	REMOTE_JOB_SIMULATE,
};

struct WorkerJobRequest {
	int jobType;
	RunProgramJobRequest jobRun;
	MSGPACK_DEFINE_ARRAY(jobType, jobRun)
};

struct WorkerJobResponse {
	int jobType;
	RunProgramJobResponse jobRun;
	MSGPACK_DEFINE_ARRAY(jobType, jobRun)
};

struct DistributeRequest {
	int waitTimeout = 0;			//in msecs
	bool includeProfileData = false;
	WorkerJobRequest job;
	MSGPACK_DEFINE_ARRAY(waitTimeout, includeProfileData, job)
};

struct DistributeResponse {
	int error = 0;
	std::string profileData;
	WorkerJobResponse jobResp;
	MSGPACK_DEFINE_ARRAY(error, profileData, jobResp)
};

struct CompleteRequest {
	std::string workerid;
	std::string jobid;
	WorkerJobResponse jobResp;
	MSGPACK_DEFINE_ARRAY(workerid, jobid, jobResp)
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
	WorkerJobRequest job;
	MSGPACK_DEFINE_ARRAY(error, job)
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

	int waitJobResult(int timeoutms);
	void distributeJob();
	int waitForAssignment(int timeoutms);
	void completeJob();
	bool isBusy();

	/* assigned job fields */
	std::string jobid;
	WorkerJobRequest job;
	WorkerJobResponse jobResp;
	std::atomic<bool> resultReady{false};
};

class DistProxyServer
{
public:
	DistProxyServer(uint16_t port);

	struct ProxyStatistics {
		int64_t totalJobDistributeRequests = 0;
		int64_t successfullyDistributedJobRequests = 0;
		int64_t failedJobDistributionRequests = 0;
		int64_t erroredJobDistributionRequests = 0;
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
	int freeWorkerCount();

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
		int freeWorkerCount();

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
