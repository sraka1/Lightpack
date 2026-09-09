/*
 * ScreenBrightnessFollower.hpp
 *
 *	Scales the LED brightness with the monitor's brightness as reported by
 *	Lunar (https://lunar.fyi) on macOS. Polls the Lunar CLI, maps the zone
 *	screen's effective brightness onto a 0..1 multiplier between the
 *	configured minimum and 1.0, and ramps toward it so changes fade.
 *
 *	Prismatik is a free, open-source software: you can redistribute it and/or
 *	modify it under the terms of the GNU General Public License as published
 *	by the Free Software Foundation, either version 2 of the License, or
 *	(at your option) any later version.
 */

#pragma once

#include <QObject>
#include <QTimer>
#include <QProcess>
#include <QString>

class ScreenBrightnessFollower : public QObject
{
	Q_OBJECT
public:
	explicit ScreenBrightnessFollower(QObject *parent = nullptr);
	static bool isAvailable();

public slots:
	void setEnabled(bool enabled);
	void setMinimumPercent(int percent);

signals:
	// Multiplier (0..1) to apply on the profile's device brightness.
	void scaleChanged(double scale);

private slots:
	void poll();
	void onPollFinished(int exitCode, QProcess::ExitStatus status);
	void rampTick();

private:
	void setTarget(double target);

	QTimer m_pollTimer;
	QTimer m_rampTimer;
	QProcess *m_process{nullptr};
	bool m_enabled{false};
	int m_minimumPercent{15};
	double m_target{1.0};
	double m_current{1.0};
	int m_failures{0};
};
