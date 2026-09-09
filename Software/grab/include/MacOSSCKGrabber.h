/*
 * MacOSSCKGrabber.h
 *
 *	ScreenCaptureKit-based display grabber for macOS 12.3+.
 *
 *	Prismatik is a free, open-source software: you can redistribute it and/or
 *	modify it under the terms of the GNU General Public License as published
 *	by the Free Software Foundation, either version 2 of the License, or
 *	(at your option) any later version.
 */

#pragma once

#include "../common/defs.h"

#ifdef MAC_OS_SCK_GRAB_SUPPORT

#include "MacOSGrabberBase.hpp"
#include <QMap>

#ifdef __OBJC__
@class MacOSNativeSCKCapture;
#else
typedef struct objc_object MacOSNativeSCKCapture;
#endif

/*!
	Grabs displays through ScreenCaptureKit (SCStream). Compared to the
	CoreGraphics and AVFoundation grabbers it:
	- receives frames already downscaled by the compositor (kScaleFactor of the
	  logical display size), so per-frame averaging is cheap;
	- uses the current screen-capture API, so macOS 15+ does not show the
	  recurring "wants to record the screen" reminder for legacy APIs;
	- keeps the last frame, so a static screen still yields a result.
*/
class MacOSSCKGrabber : public MacOSGrabberBase
{
public:
	MacOSSCKGrabber(QObject *parent, GrabberContext *context);
	virtual ~MacOSSCKGrabber();

	DECLARE_GRABBER_NAME("MacOSSCKGrabber")

public slots:
	virtual void setGrabInterval(int msec);
	virtual void startGrabbing();
	virtual void stopGrabbing();

protected slots:
	virtual GrabResult grabDisplay(const CGDirectDisplayID display, GrabbedScreen& screen);

private:
	QMap<CGDirectDisplayID, MacOSNativeSCKCapture*> _captures;
};

#endif // MAC_OS_SCK_GRAB_SUPPORT
