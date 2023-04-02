#include "bpffilter.h"

#include <thread>
#include <unordered_map>
#include <unordered_set>

#include <rpcproto.h>
#include <rpc/server.h>
#include <rpc/client.h>
#include <gtest/gtest.h>
#include <proc/readproc.h>

#include <commonpp/mainappcontext.h>

#include <QUuid>
#include <QDebug>
#include <QTimer>
#include <QElapsedTimer>
#include <QCoreApplication>

using namespace std::chrono_literals;

extern int distmain(MainAppContext &ctx);
extern int distproxymain(MainAppContext &ctx);
extern int workermain(MainAppContext &ctx);
extern int customermain(MainAppContext &ctx);

static int bpftest(MainAppContext &ctx)
{
	QCoreApplication a(ctx.argc, ctx.argv);

	BpfFilter::initbpf();
	BpfFilter f;
	if (f.open())
		throw std::runtime_error("error opening ebpf filter");
	if (f.attach())
		throw std::runtime_error("error attaching ebpf filter");
	if (f.start())
		throw std::runtime_error("error starting ebpf filter");

	return a.exec();
}

static int gtestmain(MainAppContext &ctx)
{
	::testing::InitGoogleTest(&ctx.argc, ctx.argv);
	return RUN_ALL_TESTS();
}

int main(int argc, char *argv[])
{
	auto ctx = MainAppContext::parse(argc, argv);
	ctx.addbin("bpf_test", bpftest);
	ctx.addbin("distribute", distmain);
	ctx.addbin("distproxy", distproxymain);
	ctx.addbin("distworker", workermain);
	ctx.addbin("customer", customermain);
	ctx.addbin("bpf_tools_testing", gtestmain);

	if (ctx.getCurrentBinName() == "bpf_tools") {
		if (ctx.containsArg("--show-binaries")) {
			for (const auto &key: ctx.binaries)
				qDebug("%s", key.first.data());
			return 0;
		}
		if (ctx.containsArg("--print-linking-commands")) {
			for (const auto &key: ctx.binaries)
				qDebug("ln -s bpf_tools %s", key.first.data());
			return 0;
		}
		qDebug("usage: bpf_tools [--show-binaries] [--print-linking-commands]");
		return 0;
	}

	return ctx.runBinary(ctx.binname.toStdString());
}
