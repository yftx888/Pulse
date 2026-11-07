#import <AppKit/AppKit.h>
#include "menubar_mac.h"
#include <cstdio>
#include <cstdlib>
#include <fcntl.h>
#include <sys/file.h>
#include <unistd.h>

static void ApplyLaunchArguments(PulseMenuBarController* controller) {
    NSArray<NSString*>* args=NSProcessInfo.processInfo.arguments;
    BOOL requireProxy=NO, noProbe=NO, openDetails=NO, snapshotExit=NO;
    NSString* snapshotDir=nil;
    NSString* lifecycleLog=nil;
    for (NSUInteger index=1; index<args.count; ++index) {
        NSString* arg=args[index];
        if ([arg isEqualToString:@"--require-proxy"]) requireProxy=YES;
        else if ([arg isEqualToString:@"--no-probe"]) noProbe=YES;
        else if ([arg isEqualToString:@"--open-details"]) openDetails=YES;
        else if ([arg isEqualToString:@"--snapshot-exit"]) snapshotExit=YES;
        else if ([arg isEqualToString:@"--snapshot-dir"] && index+1<args.count && ![args[index+1] hasPrefix:@"--"])
            snapshotDir=args[++index];
        else if ([arg isEqualToString:@"--lifecycle-log"] && index+1<args.count && ![args[index+1] hasPrefix:@"--"])
            lifecycleLog=args[++index];
    }
    [controller setTestHooksRequireProxy:requireProxy noProbe:noProbe openDetails:openDetails
        snapshotExit:snapshotExit snapshotDir:snapshotDir];
    [controller setLifecycleLogPath:lifecycleLog];
}

@interface PulseApplicationDelegate : NSObject <NSApplicationDelegate>
- (void)stop;
@end
@implementation PulseApplicationDelegate {
    PulseMenuBarController* _controller;
}
- (void)applicationDidFinishLaunching:(NSNotification*)notification {
    (void)notification; _controller=[[PulseMenuBarController alloc] init];
    ApplyLaunchArguments(_controller);
    if (![_controller start]) {
        [self stop];
        std::exit(EXIT_FAILURE);
    }
}
- (void)applicationWillTerminate:(NSNotification*)notification { (void)notification; [self stop]; }
- (BOOL)applicationShouldTerminateAfterLastWindowClosed:(NSApplication*)application { (void)application; return NO; }
- (void)stop { [_controller shutdown]; _controller=nil; }
@end

int main() {
    @autoreleasepool {
        NSString* directory=[NSHomeDirectory() stringByAppendingPathComponent:@"Library/Application Support/Pulse"];
        NSError* error=nil;
        if (![NSFileManager.defaultManager createDirectoryAtPath:directory withIntermediateDirectories:YES attributes:nil error:&error]) {
            std::fprintf(stderr,"Pulse: %s\n",error.localizedDescription.UTF8String); return 1;
        }
        NSString* file=[directory stringByAppendingPathComponent:@"pulse.lock"];
        const int lock=open(file.fileSystemRepresentation,O_CREAT | O_RDWR | O_CLOEXEC | O_NOFOLLOW,0600);
        if (lock<0) return 1;
        if (flock(lock,LOCK_EX | LOCK_NB)!=0) { close(lock); return 0; }
        // Never unlink the lock: another instance may already hold this inode.
        NSApplication* app=NSApplication.sharedApplication;
        [app setActivationPolicy:NSApplicationActivationPolicyAccessory];
        PulseApplicationDelegate* delegate=[[PulseApplicationDelegate alloc] init];
        app.delegate=delegate; [app run]; [delegate stop];
        flock(lock,LOCK_UN); close(lock); return 0;
    }
}
