#ifndef DISTPROXYSERVER_H
#define DISTPROXYSERVER_H

#include <rpc/server.h>

#include <mutex>
#include <unordered_set>
#include <condition_variable>

struct DistributeRequest {
	int waitTimeout;			//in msecs
	MSGPACK_DEFINE_ARRAY(waitTimeout)
};

struct DistributeResponse {
	int error;
	MSGPACK_DEFINE_ARRAY(error)
};

struct CompleteRequest {
	std::string workerid;
	std::string jobid;
	MSGPACK_DEFINE_ARRAY(workerid, jobid)
};

struct CompleteResponse {
	int error;
	MSGPACK_DEFINE_ARRAY(error)
};

struct JobRequest {
	int timeout;				//in msecs
	std::string uuid;
	MSGPACK_DEFINE_ARRAY(uuid, timeout)
};

struct JobResponse {
	int error;
	MSGPACK_DEFINE_ARRAY(error)
};

struct RegisterRequest {
	std::string uuid;
	MSGPACK_DEFINE_ARRAY(uuid)
};

struct WorkerObject {
	std::mutex m;
	std::string uuid;
	std::condition_variable cv;

	/* assigned job fields */
	std::string jobid;
	std::mutex mw;
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


protected:
	CompleteResponse serveComplete(const CompleteRequest &req);
	JobResponse serveRequest(const JobRequest &req);
	DistributeResponse serveDistribute(const DistributeRequest &req);

protected:
	rpc::server srv;
	ProxyStatistics stats;
	std::mutex globalLock;
	std::unordered_set<std::string> freeWorkers;
	std::unordered_set<std::string> busyWorkers;
	std::unordered_map<std::string, WorkerObject> registeredRunners;
};

#endif // DISTPROXYSERVER_H
