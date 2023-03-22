#ifndef FUNCTIONPROFILER_H
#define FUNCTIONPROFILER_H

#include <QElapsedTimer>

#include <string>
#include <unordered_map>

#include <limits.h>

class FunctionProfiler
{
public:
	FunctionProfiler();

	struct SectionStatistics
	{
		int64_t callCount = 0;
		int64_t totalElapsed = 0;
		int64_t maxElapsed = 0;
		int64_t minElapsed = LONG_LONG_MAX;
	};

	void restart();
	void startSection(const std::string &s);
	void endSection();
	const std::unordered_map<std::string, SectionStatistics> get() const;

	std::string serializeToString();
	void deserializeFromString(const std::string &str);

protected:
	QElapsedTimer t;
	std::string last;
	std::unordered_map<std::string, std::pair<int64_t, int64_t>> sections;
	std::unordered_map<std::string, SectionStatistics> stats;
};

#endif // FUNCTIONPROFILER_H
