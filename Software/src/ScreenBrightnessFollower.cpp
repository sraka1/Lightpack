/*
 * ScreenBrightnessFollower.cpp
 */

#include "ScreenBrightnessFollower.hpp"
#include "Settings.hpp"
#include "debug.h"

#include <QFile>
#include <QJsonDocument>
#include <QJsonArray>
#include <QJsonObject>
#include <cmath>

using namespace SettingsScope;

namespace {
	// Lunar's CLI lives inside the app bundle and talks to the running app
	// over a local socket when invoked with the "@" marker.
	const char* const kLunarBinary = "/Applications/Lunar.app/Contents/MacOS/Lunar";
	const int kPollIntervalMs = 2000;
	const int kRampIntervalMs = 50;
	const double kRampStep = 0.04;     // ~0.5 s for a full-range change
	const double kEpsilon = 0.005;
}

ScreenBrightnessFollower::ScreenBrightnessFollower(QObject *parent) : QObject(parent)
{
	m_pollTimer.setInterval(kPollIntervalMs);
	connect(&m_pollTimer, &QTimer::timeout, this, &ScreenBrightnessFollower::poll);
	m_rampTimer.setInterval(kRampIntervalMs);
	connect(&m_rampTimer, &QTimer::timeout, this, &ScreenBrightnessFollower::rampTick);
}

bool ScreenBrightnessFollower::isAvailable()
{
	return QFile::exists(QString::fromLatin1(kLunarBinary));
}

void ScreenBrightnessFollower::setEnabled(bool enabled)
{
	DEBUG_LOW_LEVEL << Q_FUNC_INFO << enabled;
	if (m_enabled == enabled)
		return;
	m_enabled = enabled;
	if (enabled) {
		if (!isAvailable()) {
			qWarning() << Q_FUNC_INFO << "Lunar not found at" << kLunarBinary << "- cannot follow screen brightness";
			m_enabled = false;
			return;
		}
		m_failures = 0;
		m_pollTimer.start();
		poll();
	} else {
		m_pollTimer.stop();
		if (m_process) { m_process->kill(); m_process->deleteLater(); m_process = nullptr; }
		setTarget(1.0);   // hand the full profile brightness back, smoothly
	}
}

void ScreenBrightnessFollower::setMinimumPercent(int percent)
{
	m_minimumPercent = qBound(0, percent, 100);
}

void ScreenBrightnessFollower::poll()
{
	if (!m_enabled || m_process)
		return;
	m_process = new QProcess(this);
	connect(m_process, qOverload<int, QProcess::ExitStatus>(&QProcess::finished), this, &ScreenBrightnessFollower::onPollFinished);
	m_process->start(QString::fromLatin1(kLunarBinary), { QStringLiteral("@"), QStringLiteral("displays"), QStringLiteral("--json") });
}

void ScreenBrightnessFollower::onPollFinished(int exitCode, QProcess::ExitStatus status)
{
	QProcess *proc = m_process;
	m_process = nullptr;
	if (!proc)
		return;
	const QByteArray out = proc->readAllStandardOutput();
	proc->deleteLater();
	if (!m_enabled)
		return;

	QJsonParseError err;
	const QJsonDocument doc = QJsonDocument::fromJson(out, &err);
	if (status != QProcess::NormalExit || exitCode != 0 || err.error != QJsonParseError::NoError) {
		if (++m_failures == 1 || m_failures % 30 == 0)
			qWarning() << Q_FUNC_INFO << "Lunar query failed (exit" << exitCode << ")" << err.errorString();
		return;
	}
	m_failures = 0;

	// Prefer the screen the zones live on (Grab/ZoneScreenIdentity is "name|manufacturer|serial").
	const QString wanted = Settings::getZoneScreenIdentity().section(QLatin1Char('|'), 0, 0).trimmed();
	QJsonArray displays;
	if (doc.isArray())
		displays = doc.array();
	else if (doc.isObject())
		for (const QJsonValue& v : doc.object()) displays.append(v);

	QJsonObject chosen;
	for (const QJsonValue& v : displays) {
		const QJsonObject o = v.toObject();
		if (chosen.isEmpty())
			chosen = o;
		if (!wanted.isEmpty() && o.value(QStringLiteral("name")).toString().contains(wanted, Qt::CaseInsensitive)) {
			chosen = o;
			break;
		}
	}
	if (chosen.isEmpty())
		return;

	const double brightness = chosen.value(QStringLiteral("brightness")).toDouble(0.0);
	const double software = qBound(0.0, chosen.value(QStringLiteral("softwareBrightness")).toDouble(1.0), 1.0);
	const double screen = qBound(0.0, brightness * software, 100.0);   // 0..100
	const double minimum = m_minimumPercent / 100.0;
	setTarget(minimum + (1.0 - minimum) * screen / 100.0);
}

void ScreenBrightnessFollower::setTarget(double target)
{
	target = qBound(0.0, target, 1.0);
	if (std::fabs(target - m_target) < kEpsilon)
		return;
	DEBUG_LOW_LEVEL << Q_FUNC_INFO << "brightness scale" << m_current << "->" << target;
	m_target = target;
	if (!m_rampTimer.isActive())
		m_rampTimer.start();
}

void ScreenBrightnessFollower::rampTick()
{
	const double delta = m_target - m_current;
	if (std::fabs(delta) <= kRampStep) {
		m_current = m_target;
		m_rampTimer.stop();
	} else {
		m_current += (delta > 0 ? kRampStep : -kRampStep);
	}
	emit scaleChanged(m_current);
}
