#include "functionprofiler.h"

#include <QDataStream>

FunctionProfiler::FunctionProfiler()
{
	t.start();
}

void FunctionProfiler::restart()
{
	t.restart();
	/* accumulate results */
	for (const auto &p: sections) {
		auto *s = &stats[p.first];
		auto dur = p.second.second - p.second.first;
		s->callCount++;
		s->totalElapsed += dur;
		s->maxElapsed = std::max(s->maxElapsed, dur);
		s->minElapsed = std::min(s->minElapsed, dur);
	}
	sections.clear();
}

void FunctionProfiler::startSection(const std::string &s)
{
	sections[s].first = t.nsecsElapsed();
	last = s;
}

void FunctionProfiler::endSection()
{
	sections[last].second = t.nsecsElapsed();
}

const std::unordered_map<std::string, FunctionProfiler::SectionStatistics> FunctionProfiler::get() const
{
	return stats;
}

std::string FunctionProfiler::serializeToString()
{
	QByteArray ba;
	QDataStream out(&ba, QIODevice::WriteOnly);
	for (const auto &[key, value]: stats) {
		out << QString::fromStdString(key);
		out << (qint64)value.callCount;
		out << (qint64)value.maxElapsed;
		out << (qint64)value.minElapsed;
		out << (qint64)value.totalElapsed;
	}
	return std::string(ba.constData(), ba.size());
}

void FunctionProfiler::deserializeFromString(const std::string &str)
{
	const QByteArray &ba = QByteArray::fromRawData(str.data(), str.size());
	QDataStream in(ba);
	while (!in.atEnd()) {
		QString key;
		in >> key;
		SectionStatistics stats;
		qint64 v;
		in >> v ; stats.callCount = v;
		in >> v ; stats.maxElapsed = v;
		in >> v ; stats.minElapsed = v;
		in >> v ; stats.totalElapsed = v;
		this->stats[key.toStdString()] = stats;
	}
}
