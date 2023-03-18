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

	int start();

protected:
	CompleteResponse serveComplete(const CompleteRequest &req);
	JobResponse serveRequest(const JobRequest &req);
	DistributeResponse serveDistribute(const DistributeRequest &req);

protected:
	rpc::server srv;
	std::mutex globalLock;
	std::unordered_set<std::string> freeWorkers;
	std::unordered_set<std::string> busyWorkers;
	std::unordered_map<std::string, WorkerObject> registeredRunners;
};

#endif // DISTPROXYSERVER_H
