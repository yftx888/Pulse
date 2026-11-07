#pragma once
#import <AppKit/AppKit.h>
#include "renderer_mac.h"

@interface PulsePanelController : NSObject
@property(nonatomic, readonly) BOOL detailsVisible;
// When YES, outside clicks, app switches and Escape leave the details panel open.
// toggleDetailsFromButton and closeDetails still close it.
@property(nonatomic) BOOL keepDetailsOpen;
@property(nonatomic, copy) void (^detailsChanged)(BOOL visible);
- (void)toggleDetailsFromButton:(NSStatusBarButton*)button components:(const pulse::ui::Components&)components;
- (void)updateDetails:(const pulse::ui::View&)view components:(const pulse::ui::Components&)components;
- (void)closeDetails;
- (void)showEvent:(const pulse::NetEvent&)event fromButton:(NSStatusBarButton*)button;
- (void)closeToast;
- (BOOL)writeDetailsPNGToURL:(NSURL*)url error:(NSError**)outError;
- (BOOL)writeToastPNGToURL:(NSURL*)url netEvent:(const pulse::NetEvent&)netEvent error:(NSError**)outError;
- (void)shutdown;
@end

// Draw view into a 2x bitmap via bitmapImageRepForCachingDisplayInRect: and
// cacheDisplayInRect:toBitmapImageRep:. Returns nil when the view is empty.
NSBitmapImageRep* PulseCachedBitmap(NSView* view, NSRect rect);
BOOL PulseWritePNG(NSBitmapImageRep* rep, NSURL* url, NSError** outError);
