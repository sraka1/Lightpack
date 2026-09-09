/*
 * LedDeviceAdalight.cpp
 *
 *	Created on: 06.09.2011
 *		Author: Mike Shatohin (brunql)
 *		Project: Lightpack
 *
 *	Lightpack is very simple implementation of the backlight for a laptop
 *
 *	Copyright (c) 2011 Mike Shatohin, mikeshatohin [at] gmail.com
 *
 *	Lightpack is free software: you can redistribute it and/or modify
 *	it under the terms of the GNU General Public License as published by
 *	the Free Software Foundation, either version 2 of the License, or
 *	(at your option) any later version.
 *
 *	Lightpack is distributed in the hope that it will be useful,
 *	but WITHOUT ANY WARRANTY; without even the implied warranty of
 *	MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.	See the
 *	GNU General Public License for more details.
 *
 *	You should have received a copy of the GNU General Public License
 *	along with this program.	If not, see <http://www.gnu.org/licenses/>.
 *
 */

#include "LedDeviceAdalight.hpp"
#include "PrismatikMath.hpp"
#include "Settings.hpp"
#include "debug.h"
#include "stdio.h"
#include <QtSerialPort/QSerialPortInfo>
#include <algorithm>

using namespace SettingsScope;

// Idle gap after which the last frame is re-sent (see m_keepAliveTimer).
static const int KeepAliveIntervalMs = 250;

LedDeviceAdalight::LedDeviceAdalight(const QString &portName, const int baudRate, QObject *parent) : AbstractLedDevice(parent)
{
	DEBUG_LOW_LEVEL << Q_FUNC_INFO;

	m_portName = portName;
	m_baudRate = baudRate;

	m_AdalightDevice = NULL;
	m_lastWillTimer = new QTimer(this);
	m_lastWillTimer->setTimerType(Qt::PreciseTimer);
	connect(m_lastWillTimer, &QTimer::timeout, this, qOverload<>(&LedDeviceAdalight::writeLastWill));
	m_keepAliveTimer = new QTimer(this);
	m_keepAliveTimer->setTimerType(Qt::PreciseTimer);
	m_keepAliveTimer->setSingleShot(true);
	m_keepAliveTimer->setInterval(KeepAliveIntervalMs);
	connect(m_keepAliveTimer, &QTimer::timeout, this, &LedDeviceAdalight::writeKeepAlive);
	// TODO: think about init m_savedColors in all ILedDevices

	DEBUG_LOW_LEVEL << Q_FUNC_INFO << "initialized";
}

LedDeviceAdalight::~LedDeviceAdalight()
{
	close();
	delete m_lastWillTimer;
	delete m_keepAliveTimer;
}

int LedDeviceAdalight::maxLedsCount()
{
	return MaximumNumberOfLeds::Adalight;
}

void LedDeviceAdalight::close()
{
	if (m_AdalightDevice == NULL)
		return;

	m_keepAliveTimer->stop();
	if (m_lastWillTimer->isActive()) {
		m_lastWillTimer->stop();
		writeLastWill(true);
	}
	m_AdalightDevice->close();

	delete m_AdalightDevice;
	m_AdalightDevice = NULL;
}

void LedDeviceAdalight::setColors(const QList<LinearRgbF> & colors)
{
	// A black frame arriving while the strip is off is a settings re-apply
	// (updateDeviceSettings / setColorSequence), not a request to light up:
	// keep it a raw off frame instead of pushing black through the colour
	// pipeline, whose minimum-luminosity floor would make it dim grey.
	const bool wasOff = m_isOff;
	m_isOff = false;
	if (wasOff && std::all_of(colors.cbegin(), colors.cend(), [](const LinearRgbF& c) { return c.r == 0.f && c.g == 0.f && c.b == 0.f; })) {
		switchOffLeds();
		return;
	}
	// Save colors for showing changes of the brightness
	m_colorsSaved = colors;

	resizeColorsBuffer(colors.count());

	applyColorModifications(colors, m_colorsBuffer);
	applyDithering(m_colorsBuffer, 8);

	m_writeBuffer.clear();
	m_writeBuffer.append(m_writeBufferHeader);

	for (int i = 0; i < m_colorsBuffer.count(); i++)
	{
		StructRgb color = m_colorsBuffer[i];

		if (m_colorSequence == QStringLiteral("RBG"))
		{
			m_writeBuffer.append(color.r);
			m_writeBuffer.append(color.b);
			m_writeBuffer.append(color.g);
		}
		else if (m_colorSequence == QStringLiteral("BRG"))
		{
			m_writeBuffer.append(color.b);
			m_writeBuffer.append(color.r);
			m_writeBuffer.append(color.g);
		}
		else if (m_colorSequence == QStringLiteral("BGR"))
		{
			m_writeBuffer.append(color.b);
			m_writeBuffer.append(color.g);
			m_writeBuffer.append(color.r);
		}
		else if (m_colorSequence == QStringLiteral("GRB"))
		{
			m_writeBuffer.append(color.g);
			m_writeBuffer.append(color.r);
			m_writeBuffer.append(color.b);
		}
		else if (m_colorSequence == QStringLiteral("GBR"))
		{
			m_writeBuffer.append(color.g);
			m_writeBuffer.append(color.b);
			m_writeBuffer.append(color.r);
		}
		else
		{
			m_writeBuffer.append(color.r);
			m_writeBuffer.append(color.g);
			m_writeBuffer.append(color.b);
		}
	}

	bool ok = writeBuffer(m_writeBuffer);

	emit commandCompleted(ok);
}

void LedDeviceAdalight::switchOffLeds()
{
	m_isOff = true;
	int count = m_colorsSaved.count();
	// Before the first setColors() (e.g. the app starts with the backlight
	// off) nothing is saved yet; still send a real black frame for the
	// configured strip, otherwise nothing at all goes out and the board keeps
	// showing whatever its sketch does on boot.
	if (count == 0)
		count = Settings::getNumberOfLeds(SupportedDevices::DeviceTypeAdalight);
	resizeColorsBuffer(count);
	m_colorsSaved.clear();

	for (int i = 0; i < count; i++)
		m_colorsSaved << LinearRgbF{};

	m_writeBuffer.clear();
	m_writeBuffer.append(m_writeBufferHeader);

	for (int i = 0; i < count; i++) {
		m_writeBuffer.append((char)0)
						.append((char)0)
						.append((char)0);
	}

	bool ok = writeBuffer(m_writeBuffer);
	emitBlackColorsUpdatedIfEnabled(count);
	emit commandCompleted(ok);
}

void LedDeviceAdalight::setRefreshDelay(int /*value*/)
{
	emit commandCompleted(true);
}

void LedDeviceAdalight::setColorDepth(int /*value*/)
{
	emit commandCompleted(true);
}

void LedDeviceAdalight::setSmoothSlowdown(int /*value*/)
{
	emit commandCompleted(true);
}

void LedDeviceAdalight::setColorSequence(const QString& value)
{
	DEBUG_LOW_LEVEL << Q_FUNC_INFO << value;

	m_colorSequence = value;
	if (m_isOff)
		switchOffLeds();
	else
		setColors(m_colorsSaved);
}

void LedDeviceAdalight::requestFirmwareVersion()
{
	emit firmwareVersion(QStringLiteral("unknown (adalight device)"));
	emit commandCompleted(true);
}

void LedDeviceAdalight::updateDeviceSettings()
{
	DEBUG_LOW_LEVEL << Q_FUNC_INFO;

	AbstractLedDevice::updateDeviceSettings();
	setColorSequence(Settings::getColorSequence(SupportedDevices::DeviceTypeAdalight));
}

void LedDeviceAdalight::open()
{
	DEBUG_LOW_LEVEL << Q_FUNC_INFO << sender();


	if (m_AdalightDevice != NULL)
		m_AdalightDevice->close();
	else
		m_AdalightDevice = new QSerialPort();

	m_AdalightDevice->setPortName(resolvePortName());

	// ReadWrite, not WriteOnly: with a write-only QSerialPort on macOS the device
	// thread's poll() loop spins at 100% CPU (reproduced with a minimal Qt program;
	// ReadWrite idles at 0%). Incoming bytes (Adalight sketches greet with "Ada\n")
	// are drained so they never accumulate.
	m_AdalightDevice->open(QIODevice::ReadWrite);
	bool ok = m_AdalightDevice->isOpen();

	// Ubuntu 10.04: on every second attempt to open the device leads to failure
	if (ok == false)
	{
		qWarning() << Q_FUNC_INFO << "Serial device" << m_AdalightDevice->portName() << "open fail, will retry. Error" << (int)m_AdalightDevice->error() << m_AdalightDevice->errorString();
		// Try one more time
		m_AdalightDevice->open(QIODevice::ReadWrite);
		ok = m_AdalightDevice->isOpen();
	}

	if (ok)
	{
		DEBUG_LOW_LEVEL << Q_FUNC_INFO << "Serial device" << m_AdalightDevice->portName() << "open";
		connect(m_AdalightDevice, &QSerialPort::readyRead, m_AdalightDevice, [this]() { m_AdalightDevice->readAll(); });
		m_AdalightDevice->clear(QSerialPort::AllDirections);
		m_lastWrite.invalidate();
		ok = m_AdalightDevice->setBaudRate(m_baudRate);//Settings::getAdalightSerialPortBaudRate());
		if (ok)
		{
			ok = m_AdalightDevice->setDataBits(QSerialPort::Data8);
			if (ok)
			{
				DEBUG_LOW_LEVEL << Q_FUNC_INFO << "Baud rate	:" << m_AdalightDevice->baudRate();
				DEBUG_LOW_LEVEL << Q_FUNC_INFO << "Data bits	:" << m_AdalightDevice->dataBits();
				DEBUG_LOW_LEVEL << Q_FUNC_INFO << "Parity		:" << m_AdalightDevice->parity();
				DEBUG_LOW_LEVEL << Q_FUNC_INFO << "Stop bits	:" << m_AdalightDevice->stopBits();
				DEBUG_LOW_LEVEL << Q_FUNC_INFO << "Flow			:" << m_AdalightDevice->flowControl();
			} else {
				qWarning() << Q_FUNC_INFO << "Set data bits 8 fail. Error" << (int)m_AdalightDevice->error() << m_AdalightDevice->errorString();
			}
		} else {
			qWarning() << Q_FUNC_INFO << "Set baud rate" << m_baudRate << "fail. Error" << (int)m_AdalightDevice->error() << m_AdalightDevice->errorString();
		}

	} else {
		qWarning() << Q_FUNC_INFO << "Serial device" << m_AdalightDevice->portName() << "open fail. Error" << (int)m_AdalightDevice->error() << m_AdalightDevice->errorString();
		DEBUG_OUT << Q_FUNC_INFO << "Available ports:";
		QList<QSerialPortInfo> availPorts = QSerialPortInfo::availablePorts();
		for(int i=0; i < availPorts.size(); i++) {
			DEBUG_OUT << Q_FUNC_INFO << availPorts[i].portName();
		}
	}

	emit openDeviceSuccess(ok);
}

// The configured port name, unless it is gone and exactly one other USB
// serial adapter (or one with the same vendor/product id as the adapter we
// last opened) is present. macOS names adapters without a serial number by
// USB location, so moving the cable to another port or hub renames it.
QString LedDeviceAdalight::resolvePortName()
{
	const QList<QSerialPortInfo> ports = QSerialPortInfo::availablePorts();
	for (const QSerialPortInfo& p : ports) {
		if (p.portName() == m_portName || p.systemLocation() == m_portName) {
			if (p.hasVendorIdentifier()) { m_lastVendorId = p.vendorIdentifier(); m_lastProductId = p.productIdentifier(); }
			return m_portName;
		}
	}
	// macOS exposes every adapter twice (tty.* and cu.*); count only the cu.*
	// call-out node, which is the one we want to open anyway.
	QList<QSerialPortInfo> usb;
	for (const QSerialPortInfo& p : ports)
		if (p.hasVendorIdentifier() && !p.portName().startsWith(QStringLiteral("tty."))
				&& !p.portName().contains(QStringLiteral("Bluetooth"), Qt::CaseInsensitive))
			usb << p;
	const QSerialPortInfo* pick = nullptr;
	if (m_lastVendorId) {
		for (const QSerialPortInfo& p : usb)
			if (p.vendorIdentifier() == m_lastVendorId && p.productIdentifier() == m_lastProductId) { pick = &p; break; }
	}
	if (!pick && usb.size() == 1)
		pick = &usb.first();
	if (!pick) {
		qWarning() << Q_FUNC_INFO << "configured port" << m_portName << "not present and no unambiguous USB serial replacement (" << usb.size() << "candidates)";
		return m_portName;
	}
	qWarning() << Q_FUNC_INFO << "configured port" << m_portName << "not present, using" << pick->systemLocation()
			   << "(vid" << Qt::hex << pick->vendorIdentifier() << "pid" << pick->productIdentifier() << ")";
	m_lastVendorId = pick->vendorIdentifier(); m_lastProductId = pick->productIdentifier();
	return pick->systemLocation();
}

void LedDeviceAdalight::writeLastWill()
{
	writeLastWill(false);
}

void LedDeviceAdalight::writeLastWill(const bool force)
{
	if (force || m_AdalightDevice->bytesToWrite() == 0) {
		DEBUG_MID_LEVEL << Q_FUNC_INFO << "Writing last will frame" << (m_isOff ? "(off)" : "");
		if (m_isOff)
			switchOffLeds();
		else
			setColors(m_colorsSaved);
	}
}

bool LedDeviceAdalight::writeBuffer(const QByteArray & buff)
{
	DEBUG_MID_LEVEL << Q_FUNC_INFO << "Hex:" << buff.toHex();

	if (m_AdalightDevice == NULL || m_AdalightDevice->isOpen() == false)
		return false;

	using namespace std::chrono_literals;
	if (m_AdalightDevice->bytesToWrite() > 0) {
		DEBUG_MID_LEVEL << Q_FUNC_INFO << "Serial bytesToWrite:" << m_AdalightDevice->bytesToWrite() << ", skipping current frame";
		// If no more writes will be done ("Send data only of colors changed")
		// re-schedule last skipped frame in case it's important (for ex a black frame to turn off)
		m_lastWillTimer->start(100ms);
		return true;
	}
	// Pace frames to the line rate. bytesToWrite() only covers QSerialPort's own
	// buffer; the OS/USB-serial driver queue behind it absorbs seconds' worth of
	// frames when we write faster than the line carries them (180 LEDs at
	// 115200 baud = ~47 ms per frame vs a 25 ms grab interval), and the strip
	// then lags 2-3 s behind the screen. Skip a frame while the previous one
	// cannot have finished transmitting (10 bits per byte on the wire). Pace
	// 10% slower than nominal: at exactly the nominal rate rounding and USB
	// packetisation still let the driver queue (about 6 KB, i.e. ~0.5 s of
	// frames, on an FTDI adapter) fill up over a minute, and that backlog is
	// pure added latency. A slightly slower cadence keeps the queue empty.
	const qint64 frameMs = (static_cast<qint64>(buff.size()) * 10 * 1000 * 11) / (qMax(1, m_baudRate) * 10) + 1;
	if (m_lastWrite.isValid() && m_lastWrite.elapsed() < frameMs) {
		const qint64 remaining = frameMs - m_lastWrite.elapsed();
		DEBUG_HIGH_LEVEL << Q_FUNC_INFO << "line busy for another" << remaining << "ms, skipping current frame";
		m_lastWillTimer->start(std::chrono::milliseconds(qMax<qint64>(1, remaining)));
		return true;
	}
	m_lastWillTimer->stop();

	int bytesWritten = m_AdalightDevice->write(buff);

	if (bytesWritten != buff.count())
	{
		qWarning() << Q_FUNC_INFO << "bytesWritten != buff.count():" << bytesWritten << buff.count() << " " << m_AdalightDevice->errorString();
		emit ioDeviceSuccess(false);
		return false;
	}

	m_lastWrite.restart();
	m_keepAliveTimer->start();
	emit ioDeviceSuccess(true);
	return true;
}

void LedDeviceAdalight::writeKeepAlive()
{
	if (m_AdalightDevice == NULL || m_AdalightDevice->isOpen() == false || m_writeBuffer.isEmpty())
		return;
	if (m_AdalightDevice->bytesToWrite() > 0) {
		m_keepAliveTimer->start();
		return;
	}
	DEBUG_HIGH_LEVEL << Q_FUNC_INFO << "re-sending last frame";
	// Not a command from LedDeviceManager, so no commandCompleted() here.
	writeBuffer(m_writeBuffer);
}

void LedDeviceAdalight::resizeColorsBuffer(int buffSize)
{
	if (m_colorsBuffer.count() == buffSize)
		return;

	m_colorsBuffer.clear();

	if (buffSize > MaximumNumberOfLeds::Adalight)
	{
		qCritical() << Q_FUNC_INFO << "buffSize > MaximumNumberOfLeds::Adalight" << buffSize << ">" << MaximumNumberOfLeds::Adalight;

		buffSize = MaximumNumberOfLeds::Adalight;
	}

	for (int i = 0; i < buffSize; i++)
	{
		m_colorsBuffer << StructRgb();
	}

	reinitBufferHeader(buffSize);
}

void LedDeviceAdalight::reinitBufferHeader(int ledsCount)
{
	m_writeBufferHeader.clear();

	// Initialize buffer header
	int ledsCountHi = ((ledsCount - 1) >> 8) & 0xff;
	int ledsCountLo = (ledsCount	- 1) & 0xff;

	m_writeBufferHeader.append((char)'A');
	m_writeBufferHeader.append((char)'d');
	m_writeBufferHeader.append((char)'a');
	m_writeBufferHeader.append((char)ledsCountHi);
	m_writeBufferHeader.append((char)ledsCountLo);
	m_writeBufferHeader.append((char)(ledsCountHi ^ ledsCountLo ^ 0x55));
}
