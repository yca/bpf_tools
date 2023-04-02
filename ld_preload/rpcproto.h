#ifndef __RPCPROTO_H
#define __RPCPROTO_H

#include <string>

#include <rpc/msgpack.hpp>

struct RunResult {
	int exitCode;
	std::string err;
	std::string out;
	MSGPACK_DEFINE_ARRAY(exitCode, err, out)
};

#endif /* __RPCPROTO_H */
