#ifndef CUSTOMPROCESS_H
#define CUSTOMPROCESS_H

#include <QProcess>

class CustomProcess : public QProcess
{
	Q_OBJECT
public:
	explicit CustomProcess(QObject *parent = nullptr);

	 virtual void setupChildProcess() override;

signals:

protected:
	int uid = 0;
	int gid = 0;
};

#endif // CUSTOMPROCESS_H
