#include "distproxyserver.h"
#include "distproxytests.h"

#include <thread>
#include <rpc/client.h>
#include <gtest/gtest.h>

#include <QUuid>
#include <QDebug>

using namespace std::chrono_literals;

TEST(DistProxyServer, ListenTest) {
	DistProxyServer srv(40001);
	EXPECT_EQ(srv.start(), 0);
}

TEST(DistProxyServer, MultipleBindTest) {
	DistProxyServer srv(40001);
	EXPECT_EQ(srv.start(), 0);
	EXPECT_ANY_THROW({
		DistProxyServer srv2(40001);
	});
}

TEST(DistProxyServer, NilRegisteredListing) {
	DistProxyServer srv(40001);
	EXPECT_EQ(srv.start(), 0);

	rpc::client c("127.0.0.1", 40001);
	RegisterListingRequest req;
	req.onlyActive = false;
	const auto &resp = c.call("get_registered", req).as<RegisterListingResponse>();
	EXPECT_EQ(resp.list.size(), 0);
}

TEST(DistProxyServer, RegisteredListing) {
	DistProxyServer srv(40001);
	EXPECT_EQ(srv.start(), 0);

	rpc::client c("127.0.0.1", 40001);

	/* register some workers */
	int count = 10;
	for (int i = 0; i < count; i++) {
		RegisterRequest req;
		req.uuid = std::to_string(i);
		int res = c.call("register", req).as<int>();
		EXPECT_EQ(res, 0);
	}

	RegisterListingRequest req;
	req.onlyActive = false;
	const auto &resp = c.call("get_registered", req).as<RegisterListingResponse>();
	EXPECT_EQ(resp.list.size(), count);
}

TEST(DistProxyServer, UnRegistering) {
	DistProxyServer srv(40001);
	EXPECT_EQ(srv.start(), 0);

	rpc::client c("127.0.0.1", 40001);

	/* register some workers */
	int count = 10;
	for (int i = 0; i < count; i++) {
		RegisterRequest req;
		req.uuid = std::to_string(i);
		int res = c.call("register", req).as<int>();
		EXPECT_EQ(res, 0);
	}

	RegisterListingRequest req;
	req.onlyActive = false;
	const auto &resp = c.call("get_registered", req).as<RegisterListingResponse>();
	EXPECT_EQ(resp.list.size(), count);
	/* unregister */
	for (const auto &uuid: resp.list) {
		RegisterRequest req;
		req.uuid = uuid;
		int res = c.call("unregister", req).as<int>();
		EXPECT_EQ(res, 0);
	}

	{
		const auto &resp = c.call("get_registered", req).as<RegisterListingResponse>();
		EXPECT_EQ(resp.list.size(), 0);
	}
}

class ConsumerContext
{
public:
	ConsumerContext(std::string remote, int port)
		: c(remote, port)
	{

	}
	int start()
	{
		thr = std::thread([this]() {
			while (requestCount--) {
				DistributeRequest req;
				req.includeProfileData = true;
				req.waitTimeout = timeout;
				const auto &res = c.call("distribute", req).as<DistributeResponse>();
				distributeResponses.push_back(res);
				std::this_thread::sleep_for(requestWaitTime * 1ms);
			}
		});
		/* wait thread to be started */
		std::this_thread::sleep_for(100ms);
		return 0;
	}
	void wait()
	{
		thr.join();
	}
	int progress()
	{
		return requestCount;
	}
	int timeout = 1000;
	int requestCount = 100;
	int requestWaitTime = 10;
	std::vector<DistributeResponse> distributeResponses;
protected:
	rpc::client c;
	std::thread thr;
};

class WorkerContext
{
public:
	WorkerContext(std::string remote, int port)
		: c(remote, port)
	{
		myuuid = QUuid::createUuid().toString(QUuid::Id128).toStdString();
	}
	int registerWorker()
	{
		RegisterRequest req;
		req.uuid = myuuid;
		if (c.call("register", req).as<int>()) {
			qDebug("error registering ourselves to the proxy");
			return -EINVAL;
		}
		return 0;
	}
	int start()
	{
		thr = std::thread([this]() {
			while (requestCount--) {
				JobRequest req;
				req.timeout = reqTimeout;
				req.uuid = myuuid;
				const auto &res = c.call("request", req).as<JobResponse>();
				requestResponses.push_back(res);
				if (!res.error){
					CompleteRequest req;
					req.workerid = myuuid;
					if (jobSimDuration)
						std::this_thread::sleep_for(jobSimDuration * 1ms);
					const auto &res = c.call("complete", req).as<CompleteResponse>();
				}
			}
		});
		/* wait thread to be started */
		std::this_thread::sleep_for(100ms);
		return 0;
	}
	void wait()
	{
		thr.join();
	}
	int progress()
	{
		return requestCount;
	}
	int reqTimeout = 100;
	int jobSimDuration = 0;
	int requestCount = 100;
	std::vector<JobResponse> requestResponses;
protected:
	rpc::client c;
	std::string myuuid;
	std::thread thr;
};

TEST(DistProxyServer, SingleWorkerTimeout) {
	DistProxyServer srv(40001);
	EXPECT_EQ(srv.start(), 0);

	rpc::client c("127.0.0.1", 40001);

	int count = 1;
	WorkerContext ctx("127.0.0.1", 40001);
	EXPECT_EQ(ctx.registerWorker(), 0);
	ctx.reqTimeout = 1;
	ctx.requestCount = count;
	EXPECT_EQ(ctx.start(), 0);
	ctx.wait();
	EXPECT_EQ(ctx.requestResponses.size(), count);
	EXPECT_EQ(ctx.requestResponses[0].error, -ETIMEDOUT);
}

TEST(DistProxyServer, SingleConsumerTimeout) {
	DistProxyServer srv(40001);
	EXPECT_EQ(srv.start(), 0);

	int count = 1;
	ConsumerContext ctx("127.0.0.1", 40001);
	ctx.timeout = 1;
	ctx.requestCount = count;
	EXPECT_EQ(ctx.start(), 0);
	ctx.wait();
	EXPECT_EQ(ctx.distributeResponses.size(), count);
	EXPECT_EQ(ctx.distributeResponses[0].error, -ENOENT);
}

TEST(DistProxy, SingleWorkerSingleConsumerSingleRequest) {
	DistProxyServer srv(40001);
	EXPECT_EQ(srv.start(), 0);

	int count = 10;
	WorkerContext wctx("127.0.0.1", 40001);
	EXPECT_EQ(wctx.registerWorker(), 0);
	wctx.reqTimeout = 1000;
	wctx.requestCount = count;
	EXPECT_EQ(wctx.start(), 0);

	ConsumerContext cctx("127.0.0.1", 40001);
	cctx.timeout = 1000;
	cctx.requestCount = count;
	cctx.requestWaitTime = 10;
	EXPECT_EQ(cctx.start(), 0);

	wctx.wait();
	cctx.wait();

	EXPECT_EQ(wctx.requestResponses.size(), count);
	EXPECT_EQ(cctx.distributeResponses.size(), count);
	for (int i = 0; i < count; i++) {
		EXPECT_EQ(wctx.requestResponses[i].error, 0);
		EXPECT_EQ(cctx.distributeResponses[i].error, 0);
	}
}

TEST(DistProxy, MultipleWorkerSingleConsumerMultipleRequest) {
	DistProxyServer srv(40001);
	EXPECT_EQ(srv.start(), 0);

	int count = 10;
	WorkerContext wctx("127.0.0.1", 40001);
	EXPECT_EQ(wctx.registerWorker(), 0);
	wctx.reqTimeout = 1000;
	wctx.requestCount = count;
	EXPECT_EQ(wctx.start(), 0);

	ConsumerContext cctx("127.0.0.1", 40001);
	cctx.timeout = 1000;
	cctx.requestCount = count;
	cctx.requestWaitTime = 10;
	EXPECT_EQ(cctx.start(), 0);

	wctx.wait();
	cctx.wait();

	EXPECT_EQ(wctx.requestResponses.size(), count);
	EXPECT_EQ(cctx.distributeResponses.size(), count);
	for (int i = 0; i < count; i++) {
		EXPECT_EQ(wctx.requestResponses[i].error, 0);
		EXPECT_EQ(cctx.distributeResponses[i].error, 0);
	}
}

TEST(DistProxy, SingleWorkerSingleConsumer) {
	QElapsedTimer et;
	et.start();

	/* start proxy server */
	DistProxyServer srv(40001);
	EXPECT_EQ(srv.start(), 0);

	WorkerContext ctx("127.0.0.1", 40001);
	EXPECT_EQ(ctx.registerWorker(), 0);
	EXPECT_EQ(ctx.start(), 0);

	ConsumerContext cctx("127.0.0.1", 40001);
	EXPECT_EQ(cctx.start(), 0);
	ctx.wait();
	cctx.wait();

	auto stats = srv.getProxyStats();
	qDebug("=== Overview ===");
	auto elapsed = et.restart();
	auto dps = (stats.successfullyDistributedJobRequests - 0) * 1000
			/ elapsed;
	auto cps = (stats.successfullyCompletedJobs - 0) * 1000
			/ elapsed;
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
}

DistProxyTests::DistProxyTests()
{

}
