#include "panel_mac.h"
#include "mac_strings.h"
#import <QuartzCore/QuartzCore.h>
#include <algorithm>
#include <cmath>

@interface PulseNonactivatingPanel : NSPanel
@end
@implementation PulseNonactivatingPanel
- (BOOL)canBecomeKeyWindow { return YES; }
- (BOOL)canBecomeMainWindow { return NO; }
@end

@interface PulseDetailView : NSView {
@public
    pulse::mac::Renderer renderer;
    pulse::ui::View data;
}
@end
@implementation PulseDetailView
- (BOOL)isFlipped { return YES; }
- (void)drawRect:(NSRect)rect {
    (void)rect;
    renderer.Full(NSGraphicsContext.currentContext.CGContext, data);
}
@end

@interface PulseToastView : NSView {
@public
    pulse::mac::Renderer renderer;
    pulse::NetEvent netEvent;
    std::wstring message;
    NSTrackingArea* _hoverTracking;
}
@property(nonatomic, copy) void (^dismiss)(void);
@property(nonatomic, copy) void (^hoverChanged)(BOOL hovering);
@end
@implementation PulseToastView
- (BOOL)isFlipped { return YES; }
- (void)drawRect:(NSRect)rect {
    (void)rect; renderer.Toast(NSGraphicsContext.currentContext.CGContext, self.bounds.size, netEvent, message);
}
- (void)updateTrackingAreas {
    [super updateTrackingAreas];
    if (_hoverTracking) [self removeTrackingArea:_hoverTracking];
    _hoverTracking = [[NSTrackingArea alloc] initWithRect:NSZeroRect
        options:NSTrackingMouseEnteredAndExited | NSTrackingActiveAlways | NSTrackingInVisibleRect
        owner:self userInfo:nil];
    [self addTrackingArea:_hoverTracking];
}
- (void)mouseEntered:(NSEvent*)event { (void)event; if (self.hoverChanged) self.hoverChanged(YES); }
- (void)mouseExited:(NSEvent*)event { (void)event; if (self.hoverChanged) self.hoverChanged(NO); }
- (void)mouseDown:(NSEvent*)event { (void)event; if (self.dismiss) self.dismiss(); }
@end

namespace {
NSPanel* MakePanel() {
    NSPanel* panel = [[PulseNonactivatingPanel alloc] initWithContentRect:NSMakeRect(0,0,320,120)
        styleMask:NSWindowStyleMaskBorderless | NSWindowStyleMaskNonactivatingPanel
        backing:NSBackingStoreBuffered defer:NO];
    panel.opaque = NO; panel.backgroundColor = NSColor.clearColor;
    panel.hasShadow = YES; panel.level = NSPopUpMenuWindowLevel;
    panel.hidesOnDeactivate = NO; panel.becomesKeyOnlyIfNeeded = YES;
    panel.collectionBehavior = NSWindowCollectionBehaviorCanJoinAllSpaces | NSWindowCollectionBehaviorFullScreenAuxiliary;
    panel.releasedWhenClosed = NO;
    return panel;
}
NSRect Anchor(NSStatusBarButton* button, NSSize size) {
    NSRect item = [button.window convertRectToScreen:[button convertRect:button.bounds toView:nil]];
    NSScreen* screen = button.window.screen ?: NSScreen.mainScreen;
    NSRect visible = screen.visibleFrame;
    size.width = std::min(size.width, visible.size.width);
    size.height = std::min(size.height, visible.size.height);
    const CGFloat x = std::clamp(NSMidX(item)-size.width/2, NSMinX(visible), NSMaxX(visible)-size.width);
    const CGFloat y = std::clamp(NSMinY(item)-size.height-5, NSMinY(visible), NSMaxY(visible)-size.height);
    return NSMakeRect(x,y,size.width,size.height);
}
NSRect ToastFrame(NSStatusBarButton* button, NSSize size) {
    NSScreen* screen = button.window.screen ?: NSScreen.mainScreen;
    const NSRect visible = screen.visibleFrame;
    const CGFloat margin = 12;
    size.width = std::min(size.width, std::max(CGFloat(0), visible.size.width - 2 * margin));
    size.height = std::min(size.height, std::max(CGFloat(0), visible.size.height - 2 * margin));
    return NSMakeRect(NSMaxX(visible) - margin - size.width,
                      NSMaxY(visible) - margin - size.height, size.width, size.height);
}
}

static NSBitmapImageRep* PulseMakeBitmap(NSInteger pixelsWide, NSInteger pixelsHigh, NSBitmapImageRep* like) {
    if (pixelsWide < 1 || pixelsHigh < 1) return nil;
    NSBitmapImageRep* rep = nil;
    if (like.colorSpaceName.length) {
        rep = [[NSBitmapImageRep alloc] initWithBitmapDataPlanes:nullptr
            pixelsWide:pixelsWide pixelsHigh:pixelsHigh
            bitsPerSample:like.bitsPerSample samplesPerPixel:like.samplesPerPixel
            hasAlpha:like.hasAlpha isPlanar:like.isPlanar
            colorSpaceName:like.colorSpaceName bitmapFormat:like.bitmapFormat
            bytesPerRow:0 bitsPerPixel:0];
    }
    if (!rep) {
        rep = [[NSBitmapImageRep alloc] initWithBitmapDataPlanes:nullptr
            pixelsWide:pixelsWide pixelsHigh:pixelsHigh
            bitsPerSample:8 samplesPerPixel:4
            hasAlpha:YES isPlanar:NO
            colorSpaceName:NSCalibratedRGBColorSpace
            bitmapFormat:NSBitmapFormatAlphaNonpremultiplied
            bytesPerRow:0 bitsPerPixel:0];
    }
    return rep;
}

NSBitmapImageRep* PulseCachedBitmap(NSView* view, NSRect rect) {
    if (!view || rect.size.width < 1.0 || rect.size.height < 1.0) return nil;
    NSBitmapImageRep* caching = [view bitmapImageRepForCachingDisplayInRect:rect];
    const NSInteger wantWide = (NSInteger)std::llround(rect.size.width * 2.0);
    const NSInteger wantHigh = (NSInteger)std::llround(rect.size.height * 2.0);
    NSBitmapImageRep* rep = nil;
    if (caching && caching.pixelsWide == wantWide && caching.pixelsHigh == wantHigh) rep = caching;
    else {
        rep = PulseMakeBitmap(wantWide, wantHigh, caching);
        if (rep) rep.size = rect.size;
    }
    if (!rep) return nil;
    [view cacheDisplayInRect:rect toBitmapImageRep:rep];
    return rep;
}

BOOL PulseWritePNG(NSBitmapImageRep* rep, NSURL* url, NSError** outError) {
    if (!rep || !url) {
        if (outError) *outError = [NSError errorWithDomain:@"Pulse" code:1
            userInfo:@{NSLocalizedDescriptionKey: @"missing bitmap or path"}];
        return NO;
    }
    NSData* data = [rep representationUsingType:NSBitmapImageFileTypePNG properties:@{}];
    if (!data) {
        if (outError) *outError = [NSError errorWithDomain:@"Pulse" code:1
            userInfo:@{NSLocalizedDescriptionKey: @"PNG encoding failed"}];
        return NO;
    }
    return [data writeToURL:url options:NSDataWritingAtomic error:outError];
}

@interface PulsePanelController ()
- (void)removeMonitors;
- (BOOL)outsideDetails;
- (void)installMonitors;
- (void)scheduleToastDismiss;
- (void)toastHoverChanged:(BOOL)hovering;
- (void)startFade;
@end

@implementation PulsePanelController {
    NSPanel* _details;
    NSPanel* _toast;
    NSScrollView* _scroll;
    PulseDetailView* _detailView;
    PulseToastView* _toastView;
    __weak NSStatusBarButton* _anchor;
    id _globalMouse;
    id _localEvents;
    id _appSwitch;
    NSTimer* _dismissTimer;
    NSTimer* _fadeTimer;
    unsigned _fadeStep;
    BOOL _toastHovered;
}
- (BOOL)detailsVisible { return _details.visible; }
- (void)removeMonitors {
    if (_globalMouse) { [NSEvent removeMonitor:_globalMouse]; _globalMouse = nil; }
    if (_localEvents) { [NSEvent removeMonitor:_localEvents]; _localEvents = nil; }
    if (_appSwitch) { [NSWorkspace.sharedWorkspace.notificationCenter removeObserver:_appSwitch]; _appSwitch = nil; }
}
- (BOOL)outsideDetails {
    const NSPoint at = NSEvent.mouseLocation;
    NSRect item = [_anchor.window convertRectToScreen:[_anchor convertRect:_anchor.bounds toView:nil]];
    return !NSPointInRect(at, _details.frame) && !NSPointInRect(at,item);
}
- (void)installMonitors {
    [self removeMonitors];
    // --open-details: ignore outside clicks, Escape and app switches.
    // The status item and closeDetails still dismiss the panel.
    if (_keepDetailsOpen) return;
    __weak PulsePanelController* weakSelf = self;
    const NSEventMask mouse = NSEventMaskLeftMouseDown | NSEventMaskRightMouseDown | NSEventMaskOtherMouseDown;
    _globalMouse = [NSEvent addGlobalMonitorForEventsMatchingMask:mouse handler:^(NSEvent* event) {
        (void)event; PulsePanelController* owner = weakSelf;
        if (owner && [owner outsideDetails]) [owner closeDetails];
    }];
    _localEvents = [NSEvent addLocalMonitorForEventsMatchingMask:mouse | NSEventMaskKeyDown handler:^NSEvent*(NSEvent* event) {
        PulsePanelController* owner = weakSelf;
        if (!owner) return event;
        if (event.type == NSEventTypeKeyDown) {
            if (event.keyCode == 53) { [owner closeDetails]; return nil; }
        } else if ([owner outsideDetails]) [owner closeDetails];
        return event;
    }];
    _appSwitch = [NSWorkspace.sharedWorkspace.notificationCenter addObserverForName:NSWorkspaceDidActivateApplicationNotification
        object:nil queue:NSOperationQueue.mainQueue usingBlock:^(NSNotification* note) {
            NSRunningApplication* activated = note.userInfo[NSWorkspaceApplicationKey];
            if (activated.processIdentifier == NSProcessInfo.processInfo.processIdentifier) return;
            [weakSelf closeDetails];
        }];
}
- (void)toggleDetailsFromButton:(NSStatusBarButton*)button components:(const pulse::ui::Components&)components {
    if (self.detailsVisible) { [self closeDetails]; return; }
    [self closeToast]; _anchor = button;
    if (!_details) {
        _details = MakePanel();
        _scroll = [[NSScrollView alloc] initWithFrame:NSMakeRect(0,0,320,120)];
        _scroll.drawsBackground = NO; _scroll.hasVerticalScroller = YES; _scroll.autohidesScrollers = YES;
        _scroll.scrollerStyle = NSScrollerStyleOverlay; _scroll.borderType = NSNoBorder;
        _scroll.wantsLayer = YES; _scroll.layer.cornerRadius = 16; _scroll.layer.masksToBounds = YES;
        _detailView = [[PulseDetailView alloc] initWithFrame:NSMakeRect(0,0,320,120)];
        _scroll.documentView = _detailView; _details.contentView = _scroll;
    }
    _detailView->renderer.Configure(components);
    const NSSize size = NSMakeSize(_detailView->renderer.Width(), _detailView->renderer.Height());
    [_details setFrame:Anchor(button,size) display:NO];
    [_detailView setFrameSize:size]; [_detailView scrollPoint:NSZeroPoint];
    // Nonactivating panel receives Escape without activating Pulse or requesting
    // Accessibility permission/global keyboard monitoring. Main app stays active.
    [_details makeKeyAndOrderFront:nil];
    [self installMonitors];
    if (self.detailsChanged) self.detailsChanged(YES); // Immediate Sample(true).
}
- (void)updateDetails:(const pulse::ui::View&)view components:(const pulse::ui::Components&)components {
    if (!self.detailsVisible) return;
    _detailView->data = view; _detailView->renderer.Configure(components);
    const NSSize size = NSMakeSize(_detailView->renderer.Width(), _detailView->renderer.Height());
    [_details setFrame:Anchor(_anchor,size) display:NO]; [_detailView setFrameSize:size];
    _detailView.needsDisplay = YES;
}
- (void)closeDetails {
    const BOOL visible = self.detailsVisible;
    [_details orderOut:nil]; [self removeMonitors];
    if (visible && self.detailsChanged) self.detailsChanged(NO);
}
- (void)showEvent:(const pulse::NetEvent&)event fromButton:(NSStatusBarButton*)button {
    [self closeToast];
    if (!_toast) {
        _toast = MakePanel();
        _toastView = [[PulseToastView alloc] initWithFrame:NSMakeRect(0,0,320,120)];
        _toastView.wantsLayer = YES; _toastView.layer.cornerRadius = 16; _toastView.layer.masksToBounds = YES;
        __weak PulsePanelController* weakSelf = self;
        _toastView.dismiss = ^{ [weakSelf closeToast]; };
        _toastView.hoverChanged = ^(BOOL hovering) { [weakSelf toastHoverChanged:hovering]; };
        _toast.contentView = _toastView;
    }
    _toastView->netEvent = event; _toastView->message = pulse::ui::NetworkEventText(event);
    const CGFloat width = 340;
    const CGFloat height = _toastView->renderer.ToastHeight(_toastView->message,width);
    [_toast setFrame:ToastFrame(button,NSMakeSize(width,height)) display:NO];
    _toast.alphaValue = 1; _toastView.needsDisplay = YES; [_toast orderFrontRegardless];
    _toastHovered = NSPointInRect(NSEvent.mouseLocation, _toast.frame);
    [self scheduleToastDismiss];
}
- (void)scheduleToastDismiss {
    [_dismissTimer invalidate]; _dismissTimer = nil;
    if (!_toast.visible || _toastHovered) return;
    __weak PulsePanelController* weakSelf = self;
    _dismissTimer = [NSTimer timerWithTimeInterval:8.0 repeats:NO block:^(NSTimer* timer) {
        (void)timer; PulsePanelController* owner = weakSelf;
        if (owner) [owner startFade];
    }];
    [NSRunLoop.mainRunLoop addTimer:_dismissTimer forMode:NSRunLoopCommonModes];
}
- (void)toastHoverChanged:(BOOL)hovering {
    if (!_toast.visible) return;
    _toastHovered = hovering;
    [_dismissTimer invalidate]; _dismissTimer = nil;
    [_fadeTimer invalidate]; _fadeTimer = nil;
    _toast.alphaValue = 1;
    if (!hovering) [self scheduleToastDismiss];
}
- (void)startFade {
    [_dismissTimer invalidate]; _dismissTimer = nil;
    [_fadeTimer invalidate]; _fadeTimer = nil;
    if (!_toast.visible || _toastHovered) return;
    _fadeStep = 0;
    __weak PulsePanelController* weakSelf = self;
    _fadeTimer = [NSTimer timerWithTimeInterval:.025 repeats:YES block:^(NSTimer* timer) {
        PulsePanelController* owner = weakSelf;
        if (!owner) { [timer invalidate]; return; }
        ++owner->_fadeStep; owner->_toast.alphaValue = std::max(0.0,1-owner->_fadeStep/16.0);
        if (owner->_fadeStep >= 16u) [owner closeToast];
    }];
    [NSRunLoop.mainRunLoop addTimer:_fadeTimer forMode:NSRunLoopCommonModes];
}
- (void)closeToast {
    [_dismissTimer invalidate]; _dismissTimer = nil; [_fadeTimer invalidate]; _fadeTimer = nil;
    [_toast orderOut:nil]; _toast.alphaValue = 1; _toastHovered = NO;
}
- (BOOL)writeDetailsPNGToURL:(NSURL*)url error:(NSError**)outError {
    if (outError) *outError = nil;
    if (!self.detailsVisible || !_detailView) {
        if (outError) *outError = [NSError errorWithDomain:@"Pulse" code:2
            userInfo:@{NSLocalizedDescriptionKey: @"details panel is not open"}];
        return NO;
    }
    // Document view bounds cover the full card, including the scrolled-away part.
    NSBitmapImageRep* rep = PulseCachedBitmap(_detailView, _detailView.bounds);
    if (!rep) {
        if (outError) *outError = [NSError errorWithDomain:@"Pulse" code:2
            userInfo:@{NSLocalizedDescriptionKey: @"details bitmap failed"}];
        return NO;
    }
    return PulseWritePNG(rep, url, outError);
}
- (BOOL)writeToastPNGToURL:(NSURL*)url netEvent:(const pulse::NetEvent&)netEvent error:(NSError**)outError {
    if (outError) *outError = nil;
    const CGFloat width = 340;
    PulseToastView* card = [[PulseToastView alloc] initWithFrame:NSZeroRect];
    card->netEvent = netEvent;
    card->message = pulse::ui::NetworkEventText(netEvent);
    const CGFloat height = card->renderer.ToastHeight(card->message, width);
    NSWindow* offscreen = [[NSWindow alloc] initWithContentRect:NSMakeRect(-20000, -20000, width, height)
        styleMask:NSWindowStyleMaskBorderless backing:NSBackingStoreBuffered defer:NO];
    offscreen.releasedWhenClosed = NO;
    offscreen.opaque = NO;
    offscreen.backgroundColor = NSColor.clearColor;
    offscreen.hasShadow = NO;
    offscreen.alphaValue = 0;
    offscreen.ignoresMouseEvents = YES;
    offscreen.contentView = card;
    // Parked off-screen at alpha 0 so cacheDisplay has a window without showing the card.
    [offscreen orderBack:nil];
    NSBitmapImageRep* rep = PulseCachedBitmap(card, card.bounds);
    [offscreen orderOut:nil];
    offscreen.contentView = nil;
    if (!rep) {
        if (outError) *outError = [NSError errorWithDomain:@"Pulse" code:2
            userInfo:@{NSLocalizedDescriptionKey: @"toast bitmap failed"}];
        return NO;
    }
    return PulseWritePNG(rep, url, outError);
}
- (void)shutdown { self.detailsChanged = nil; [self closeDetails]; [self closeToast]; }
- (void)dealloc { [self removeMonitors]; [_dismissTimer invalidate]; [_fadeTimer invalidate]; }
@end
