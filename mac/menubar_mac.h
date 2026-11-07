#pragma once
#import <AppKit/AppKit.h>
@interface PulseMenuBarController : NSObject
- (void)setTestHooksRequireProxy:(BOOL)requireProxy
                         noProbe:(BOOL)noProbe
                     openDetails:(BOOL)openDetails
                    snapshotExit:(BOOL)snapshotExit
                     snapshotDir:(NSString*)snapshotDir;
// Opt-in local JSONL diagnostics. Creates a new file; never overwrites one.
- (void)setLifecycleLogPath:(NSString*)path;
- (BOOL)start;
- (void)shutdown;
- (void)menuAction:(NSMenuItem*)item;
@end
