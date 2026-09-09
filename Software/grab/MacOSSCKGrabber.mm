/*
 * MacOSSCKGrabber.mm
 *
 *	ScreenCaptureKit-based display grabber for macOS 12.3+.
 *	Structure mirrors MacOSAVGrabber.mm; compiled without ARC like the rest of
 *	the macOS grabbers (manual retain/release).
 */

#include "MacOSSCKGrabber.h"

#ifdef MAC_OS_SCK_GRAB_SUPPORT

#include "debug.h"
#include "GrabberContext.hpp"
#include "GrabWidget.hpp"
#import <Foundation/Foundation.h>
#import <ScreenCaptureKit/ScreenCaptureKit.h>
#import <CoreMedia/CoreMedia.h>
#import <CoreVideo/CoreVideo.h>

namespace {
	// Output size relative to the *logical* display size. GrabberBase::grab()
	// multiplies zone rects (logical, global) by GrabbedScreen::scale, so this
	// is exactly the value it needs, independent of the backing scale factor.
	static const double kScaleFactor = 0.25;
	static const int kQueueDepth = 3;
}

@interface MacOSNativeSCKCapture : NSObject <SCStreamOutput, SCStreamDelegate>
- (instancetype) initWithDisplay:(CGDirectDisplayID)display;
- (void) startWithFramerate:(double)fps;
- (void) stop;
- (void) setMaximumFramerate:(double)fps;
// Returns a +1 retained reference to the most recent frame (or NULL) and the
// last stream error, if any.
- (CVPixelBufferRef) copyLastPixelBuffer:(NSError **)error;
@end

@implementation MacOSNativeSCKCapture {
	CGDirectDisplayID		_display;
	SCStream*				_stream;
	SCStreamConfiguration*	_config;
	CVPixelBufferRef		_current;
	NSError*				_error;
	dispatch_queue_t		_outputQueue;
	size_t					_outWidth;
	size_t					_outHeight;
	double					_fps;
}

- (instancetype) initWithDisplay:(CGDirectDisplayID)display
{
	self = [super init];
	if (self) {
		_display = display;
		const CGRect bounds = CGDisplayBounds(display);
		// even dimensions keep every pixel format happy
		_outWidth  = ((size_t)(bounds.size.width  * kScaleFactor)) & ~(size_t)1;
		_outHeight = ((size_t)(bounds.size.height * kScaleFactor)) & ~(size_t)1;
		if (_outWidth < 2) _outWidth = 2;
		if (_outHeight < 2) _outHeight = 2;
		_fps = 1.0;
		_outputQueue = dispatch_queue_create("com.prismatik.sckcapture.output", DISPATCH_QUEUE_SERIAL);
	}
	return self;
}

- (void) dealloc
{
	[self stop];
	[_error release];
	dispatch_release(_outputQueue);
	[super dealloc];
}

- (size_t) outputWidth  { return _outWidth; }
- (size_t) outputHeight { return _outHeight; }

- (void) applyFramerateToConfig
{
	const double fps = _fps > 0.0 ? _fps : 1.0;
	_config.minimumFrameInterval = CMTimeMake(1, (int32_t)MAX(1.0, fps));
}

- (void) startWithFramerate:(double)fps
{
	_fps = fps;
	// SCShareableContent is the modern permission touchpoint: the first call
	// prompts once for Screen Recording, later calls are silent.
	[SCShareableContent getShareableContentExcludingDesktopWindows:NO
												  onScreenWindowsOnly:NO
													completionHandler:^(SCShareableContent* content, NSError* error) {
		if (error || !content) {
			qWarning() << "MacOSNativeSCKCapture: shareable content failed:" << (error ? error.localizedDescription.UTF8String : "no content");
			@synchronized (self) { [_error release]; _error = [error retain]; }
			return;
		}
		SCDisplay* target = nil;
		for (SCDisplay* d in content.displays) {
			if (d.displayID == _display) { target = d; break; }
		}
		if (!target) {
			qWarning() << "MacOSNativeSCKCapture: display" << _display << "not shareable";
			return;
		}
		SCContentFilter* filter = [[SCContentFilter alloc] initWithDisplay:target excludingWindows:@[]];
		SCStreamConfiguration* config = [[SCStreamConfiguration alloc] init];
		config.width = _outWidth;
		config.height = _outHeight;
		config.pixelFormat = kCVPixelFormatType_32BGRA;
		config.queueDepth = kQueueDepth;
		config.showsCursor = NO;
		@synchronized (self) {
			[_config release];
			_config = config;
			[self applyFramerateToConfig];
		}

		SCStream* stream = [[SCStream alloc] initWithFilter:filter configuration:config delegate:self];
		[filter release];
		NSError* outErr = nil;
		if (![stream addStreamOutput:self type:SCStreamOutputTypeScreen sampleHandlerQueue:_outputQueue error:&outErr]) {
			qWarning() << "MacOSNativeSCKCapture: addStreamOutput failed:" << (outErr ? outErr.localizedDescription.UTF8String : "?");
			[stream release];
			return;
		}
		@synchronized (self) {
			[_stream release];
			_stream = stream;
		}
		[stream startCaptureWithCompletionHandler:^(NSError* startErr) {
			if (startErr) {
				qWarning() << "MacOSNativeSCKCapture: startCapture failed:" << startErr.localizedDescription.UTF8String;
				@synchronized (self) { [_error release]; _error = [startErr retain]; }
			} else {
				DEBUG_LOW_LEVEL << "MacOSNativeSCKCapture: capturing display" << _display << "at" << _outWidth << "x" << _outHeight;
			}
		}];
	}];
}

- (void) stop
{
	SCStream* stream = nil;
	@synchronized (self) {
		stream = _stream;
		_stream = nil;
		[_config release];
		_config = nil;
	}
	if (stream) {
		[stream stopCaptureWithCompletionHandler:^(NSError* err) {
			Q_UNUSED(err);
			[stream release];
		}];
	}
	dispatch_sync(_outputQueue, ^{
		@synchronized (self) {
			if (_current) { CVPixelBufferRelease(_current); _current = NULL; }
		}
	});
}

- (void) setMaximumFramerate:(double)fps
{
	_fps = fps;
	@synchronized (self) {
		if (_stream && _config) {
			[self applyFramerateToConfig];
			[_stream updateConfiguration:_config completionHandler:nil];
		}
	}
}

- (void) stream:(SCStream *)stream didOutputSampleBuffer:(CMSampleBufferRef)sampleBuffer ofType:(SCStreamOutputType)type
{
	Q_UNUSED(stream);
	if (type != SCStreamOutputTypeScreen || !sampleBuffer)
		return;
	// Only complete frames carry pixels worth averaging.
	CFArrayRef attachments = CMSampleBufferGetSampleAttachmentsArray(sampleBuffer, false);
	if (attachments && CFArrayGetCount(attachments) > 0) {
		NSDictionary* info = (NSDictionary*)CFArrayGetValueAtIndex(attachments, 0);
		NSNumber* status = info[SCStreamFrameInfoStatus];
		if (status && status.integerValue != SCFrameStatusComplete)
			return;
	}
	CVPixelBufferRef pb = CMSampleBufferGetImageBuffer(sampleBuffer);
	if (!pb)
		return;
	@synchronized (self) {
		if (_current) CVPixelBufferRelease(_current);
		_current = CVPixelBufferRetain(pb);
	}
}

- (void) stream:(SCStream *)stream didStopWithError:(NSError *)error
{
	Q_UNUSED(stream);
	qWarning() << "MacOSNativeSCKCapture: stream stopped:" << (error ? error.localizedDescription.UTF8String : "no error");
	@synchronized (self) { [_error release]; _error = [error retain]; }
}

- (CVPixelBufferRef) copyLastPixelBuffer:(NSError **)error
{
	CVPixelBufferRef buffer = NULL;
	@synchronized (self) {
		buffer = CVPixelBufferRetain(_current);
		if (error)
			*error = _error;
	}
	return buffer;
}

@end

#pragma mark - MacOSSCKScreenData

namespace {
	struct MacOSSCKScreenData : public MacOSGrabberBase::MacOSScreenData
	{
		MacOSSCKScreenData() = default;
		~MacOSSCKScreenData() { setImageRef(nullptr); }

		void setImageRef(CVPixelBufferRef ref)
		{
			if (ref == displayImageRef)
				return;
			if (displayImageRef) {
				CVPixelBufferUnlockBaseAddress(displayImageRef, kCVPixelBufferLock_ReadOnly);
				CVPixelBufferRelease(displayImageRef);
			}
			displayImageRef = CVPixelBufferRetain(ref);
			if (displayImageRef)
				CVPixelBufferLockBaseAddress(displayImageRef, kCVPixelBufferLock_ReadOnly);
		}

		CVPixelBufferRef displayImageRef{nullptr};
	};

	void toGrabbedScreen(CVPixelBufferRef imageRef, GrabbedScreen& screen)
	{
		if (!screen.associatedData)
			screen.associatedData = new MacOSSCKScreenData;
		((MacOSSCKScreenData*)screen.associatedData)->setImageRef(imageRef);
		screen.bytesPerRow = CVPixelBufferGetBytesPerRow(imageRef);
		screen.scale = kScaleFactor;
		screen.imgData = (unsigned char*)CVPixelBufferGetBaseAddress(imageRef);
		screen.imgDataSize = CVPixelBufferGetHeight(imageRef) * screen.bytesPerRow;
	}
}

#pragma mark - MacOSSCKGrabber

MacOSSCKGrabber::MacOSSCKGrabber(QObject *parent, GrabberContext *context):
	MacOSGrabberBase(parent, context)
{
}

MacOSSCKGrabber::~MacOSSCKGrabber()
{
	stopGrabbing();
}

void MacOSSCKGrabber::setGrabInterval(int msec)
{
	DEBUG_LOW_LEVEL << Q_FUNC_INFO << this->metaObject()->className();
	m_timer->setInterval(msec);
	const double framerate = msec > 0 ? 1000.0 / msec : 1.0;
	foreach (MacOSNativeSCKCapture* capture, _captures) {
		[capture setMaximumFramerate:framerate];
	}
}

void MacOSSCKGrabber::startGrabbing()
{
	DEBUG_LOW_LEVEL << Q_FUNC_INFO << this->metaObject()->className();
	grabScreensCount = 0;

	if (_screensWithWidgets.empty()) {
		QList<ScreenInfo> screens2Grab;
		screensWithWidgets(&screens2Grab, *_context->grabWidgets);
		reallocate(screens2Grab);
	}

	foreach (MacOSNativeSCKCapture* capture, _captures) {
		[capture stop];
		[capture release];
	}
	_captures.clear();

	const double framerate = m_timer->interval() > 0 ? 1000.0 / m_timer->interval() : 1.0;
	foreach (const GrabbedScreen& grabScreen, _screensWithWidgets) {
		CGDirectDisplayID display = static_cast<CGDirectDisplayID>(reinterpret_cast<intptr_t>(grabScreen.screenInfo.handle));
		MacOSNativeSCKCapture* capture = [[MacOSNativeSCKCapture alloc] initWithDisplay:display];
		_captures.insert(display, capture);
		[capture startWithFramerate:framerate];
	}

	m_timer->start();
}

void MacOSSCKGrabber::stopGrabbing()
{
	DEBUG_LOW_LEVEL << Q_FUNC_INFO << this->metaObject()->className();
	DEBUG_MID_LEVEL << "grabbed" << grabScreensCount << "frames";
	m_timer->stop();

	foreach (MacOSNativeSCKCapture* capture, _captures) {
		[capture stop];
		[capture release];
	}
	_captures.clear();
}

GrabResult MacOSSCKGrabber::grabDisplay(const CGDirectDisplayID display, GrabbedScreen& screen)
{
	MacOSNativeSCKCapture* capture = _captures.value(display, nullptr);
	if (capture == nullptr) {
		qCritical() << Q_FUNC_INFO << "No capture found for display";
		return GrabResultError;
	}

	NSError* error = nil;
	CVPixelBufferRef imageRef = [capture copyLastPixelBuffer:&error];
	if (!imageRef) {
		if (error) {
			qCritical() << Q_FUNC_INFO << "capture error: " << error.localizedDescription.UTF8String;
			return GrabResultError;
		}
		return GrabResultFrameNotReady;
	}

	toGrabbedScreen(imageRef, screen);
	CVPixelBufferRelease(imageRef);
	return GrabResultOk;
}

#endif // MAC_OS_SCK_GRAB_SUPPORT
