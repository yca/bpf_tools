#include "customprocess.h"

#include <QDebug>

#include <grp.h>
#include <stdlib.h>
#include <unistd.h>

CustomProcess::CustomProcess(QObject *parent)
	: QProcess{parent}
{
	char *var = getenv("SUDO_UID");
	if (var)
		uid = QString::fromUtf8(var).toInt();
	var = getenv("SUDO_GID");
	if (var)
		gid = QString::fromUtf8(var).toInt();
	qDebug() << "will drop privieleges to" << uid << gid;
}

void CustomProcess::setupChildProcess()
{
	// Drop all privileges in the child process, and enter
		 // a chroot jail.
	if (getuid() == 0) {
		::setgroups(0, 0);
		//::chroot("/etc/safe");
		//::chdir("/");
		//::setgid(safeGid);
		//::setuid(safeUid);
		//::umask(0);
		/* process is running as root, drop privileges */
		if (setgid(gid) != 0)
			printf("setgid: Unable to drop group privileges: %s\n", strerror(errno));
		if (setuid(uid) != 0)
			printf("setuid: Unable to drop user privileges: %s\n", strerror(errno));
	}
}
