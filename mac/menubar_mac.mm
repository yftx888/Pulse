#include "menubar_mac.h"
#include "panel_mac.h"
#include "metrics_mac.h"
#include "netprobe_mac.h"
#include "mac_strings.h"
#include "compact_readouts.h"
#import <ServiceManagement/ServiceManagement.h>
#include <algorithm>
#include <chrono>
#include <cerrno>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <fcntl.h>
#include <memory>
#include <unistd.h>
#include <vector>

using namespace pulse;
using namespace pulse::ui;
using namespace pulse::mac;

@interface PulseStatusView : NSView {
    CompactReadouts _readouts;
    CGFloat _powerWidth, _timeWidth;
}
@property(nonatomic, copy) void (^clicked)(BOOL menu);
@property(nonatomic, strong) NSColor* snapshotFill;
- (CGFloat)update:(const CompactReadouts&)readouts;
- (void)setTextColors:(const TextColors&)colors;
- (NSBitmapImageRep*)bitmapWithAppearanceName:(NSString*)appearanceName fill:(NSColor*)fillColor;
@end
@implementation PulseStatusView
- (BOOL)isFlipped { return YES; }
- (BOOL)acceptsFirstMouse:(NSEvent*)event { (void)event; return YES; }
- (BOOL)isAccessibilityElement { return YES; }
- (NSString*)accessibilityRole { return NSAccessibilityButtonRole; }
- (BOOL)accessibilityPerformPress { if (self.clicked) self.clicked(NO); return YES; }
- (void)mouseDown:(NSEvent*)event { if (self.clicked) self.clicked((event.modifierFlags & NSEventModifierFlagControl) != 0); }
- (void)rightMouseDown:(NSEvent*)event { (void)event; if (self.clicked) self.clicked(YES); }
- (void)viewDidChangeEffectiveAppearance { [super viewDidChangeEffectiveAppearance]; self.needsDisplay = YES; }
- (CGFloat)update:(const CompactReadouts&)readouts {
    if (!(_readouts==readouts) || _powerWidth==0) {
        _readouts=readouts;
        self.needsDisplay=YES;
    }
    NSFont* font=[NSFont monospacedDigitSystemFontOfSize:12 weight:NSFontWeightRegular];
    // Monospaced digits keep normal updates stable; reserve only actual text.
    _powerWidth=TextWidth(_readouts.watts,font);
    _timeWidth=TextWidth(_readouts.remaining,font);
    self.accessibilityLabel=ToNSString(_readouts.watts+L" "+_readouts.remaining);
    return 6+_powerWidth+4+_timeWidth;
}
- (void)setTextColors:(const TextColors&)colors { (void)colors; }
- (NSBitmapImageRep*)bitmapWithAppearanceName:(NSString*)appearanceName fill:(NSColor*)fillColor {
    NSAppearance* saved=self.appearance;
    self.snapshotFill=fillColor;
    self.appearance=[NSAppearance appearanceNamed:appearanceName];
    NSBitmapImageRep* rep=PulseCachedBitmap(self, self.bounds);
    self.appearance=saved;
    self.snapshotFill=nil;
    self.needsDisplay=YES;
    return rep;
}
- (void)drawRect:(NSRect)dirty {
    (void)dirty;
    if (self.snapshotFill) {
        [self.snapshotFill setFill];
        NSRectFill(self.bounds);
    }
    CGContextRef context=NSGraphicsContext.currentContext.CGContext;
    NSFont* font=[NSFont monospacedDigitSystemFontOfSize:12 weight:NSFontWeightRegular];
    DrawText(context,_readouts.watts,pulse::mac::Rect(3,0,_powerWidth,self.bounds.size.height),font,NSColor.labelColor,true);
    DrawText(context,_readouts.remaining,pulse::mac::Rect(3+_powerWidth+4,0,_timeWidth,self.bounds.size.height),font,NSColor.labelColor,true);
}
@end

namespace {
void Alert(Key key, const std::wstring& detail = {}) {
    NSAlert* alert=[[NSAlert alloc] init]; alert.messageText=ToNSString(T(key));
    alert.informativeText=ToNSString(detail); [alert runModal];
}
// Menu items carry data instead of blocks, so NSMenu cannot retain the controller.
NSMenuItem* Item(NSMenu* menu, const std::wstring& title, id target, NSString* action, NSInteger value=0, NSInteger extra=0) {
    NSMenuItem* item=[[NSMenuItem alloc] initWithTitle:ToNSString(title) action:@selector(menuAction:) keyEquivalent:@""];
    item.target=target; item.representedObject=@{@"action":action,@"value":@(value),@"extra":@(extra)};
    [menu addItem:item]; return item;
}
NSMenu* Sub(NSMenu* parent, Key key) {
    NSMenu* menu=[[NSMenu alloc] initWithTitle:ToNSString(T(key))]; menu.autoenablesItems=NO;
    NSMenuItem* item=[[NSMenuItem alloc] initWithTitle:menu.title action:nil keyEquivalent:@""];
    item.submenu=menu; [parent addItem:item]; return menu;
}
void Checked(NSMenuItem* item,bool checked) { item.state=checked?NSControlStateValueOn:NSControlStateValueOff; }
const wchar_t* PowerTraceName(PowerTrace trace) {
    switch (trace) {
    case PowerTrace::Package: return L"Package";
    case PowerTrace::BatteryWhole: return L"BatteryWhole";
    case PowerTrace::BatterySide: return L"BatterySide";
    case PowerTrace::Unknown: return L"Unknown";
    }
    return L"Unknown";
}
const wchar_t* PowerModeName(PowerMode mode) {
    switch (mode) {
    case PowerMode::NoBattery: return L"NoBattery";
    case PowerMode::Discharging: return L"Discharging";
    case PowerMode::Charging: return L"Charging";
    case PowerMode::AcIdle: return L"AcIdle";
    case PowerMode::Unknown: return L"Unknown";
    }
    return L"Unknown";
}
const wchar_t* NetStateName(NetState state) {
    switch (state) {
    case NetState::Online: return L"Online";
    case NetState::Slow: return L"Slow";
    case NetState::Offline: return L"Offline";
    case NetState::Checking: return L"Checking";
    }
    return L"Checking";
}
NSString* PulseNumber(double value) {
    if (!std::isfinite(value)) return @"--";
    return [NSString stringWithFormat:@"%.3f", value];
}
NSString* YesNo(BOOL value) { return value ? @"yes" : @"no"; }
NSColor* SnapshotFill(unsigned rgb) {
    return [NSColor colorWithSRGBRed:((rgb >> 16) & 0xff) / 255.0
                               green:((rgb >> 8) & 0xff) / 255.0
                                blue:(rgb & 0xff) / 255.0
                               alpha:1];
}
void AppendMenuItems(NSMenu* menu, NSInteger depth, NSMutableString* text) {
    if (!menu || !text) return;
    for (NSMenuItem* item in menu.itemArray) {
        for (NSInteger level = 0; level < depth; ++level) [text appendString:@"  "];
        if ([item isSeparatorItem]) {
            [text appendString:@"----\n"];
            continue;
        }
        NSString* state = @"off";
        if (item.state == NSControlStateValueOn) state = @"on";
        else if (item.state == NSControlStateValueMixed) state = @"mixed";
        [text appendFormat:@"%@\tstate=%@\tenabled=%@\n", item.title ?: @"", state, item.enabled ? @"yes" : @"no"];
        if (item.submenu) AppendMenuItems(item.submenu, depth + 1, text);
    }
}
}

@interface PulseMenuBarController ()
- (void)sample;
- (void)restartTimer;
- (void)handleEvent:(const NetEvent&)event;
- (void)menuAction:(NSMenuItem*)item;
- (NSMenu*)buildMenu;
- (void)showMenu;
- (void)settingsChanged;
- (void)noteMenuSample;
- (void)beginSnapshot;
- (void)scheduleSnapshotFinish;
- (void)finishSnapshot;
- (void)requestSnapshotExit;
- (void)reportURL:(NSURL*)url error:(NSError*)error fallback:(NSString*)fallback;
- (void)writePNG:(NSBitmapImageRep*)rep name:(NSString*)name;
- (void)writeText:(NSString*)text name:(NSString*)name;
- (NSString*)menuTreeText;
- (NSString*)snapshotStateTextWithBatterySource:(const wchar_t*)batterySource
                                   systemSource:(const wchar_t*)systemSource
                                       snapshot:(const Snapshot&)snapshot;
- (void)saveSettings;
- (void)updateProbeEnabled;
- (void)toggleLogin;
- (void)chooseColor:(NSInteger)index;
- (void)colorChanged:(id)sender;
- (void)diagnose;
- (void)willSleep:(NSNotification*)note;
- (void)didWake:(NSNotification*)note;
- (void)screensSleep:(NSNotification*)note;
- (void)screensWake:(NSNotification*)note;
- (void)recordLifecycle:(NSString*)event;
@end
namespace {
void ReceiveEvent(const NetEvent& event,void* user) {
    __weak PulseMenuBarController* weakOwner=(__bridge PulseMenuBarController*)user;
    const NetEvent copy=event;
    dispatch_async(dispatch_get_main_queue(),^{ [weakOwner handleEvent:copy]; });
}
}
@implementation PulseMenuBarController {
    AppSettings _settings;
    std::unique_ptr<Metrics> _metrics;
    Model _detailModel;
    PowerTrace _trace;
    Snapshot _previousSnapshot;
    NSStatusItem* _item;
    PulseStatusView* _statusView;
    PulsePanelController* _panels;
    NSTimer* _timer;
    NSTimer* _colorSaveTimer;
    BOOL _colorSavePending;
    BOOL _screenAsleep, _systemAsleep, _stopping, _forcePaint, _diagnosing;
    NSInteger _colorIndex;
    std::chrono::steady_clock::time_point _startedAt;
    int _menuSamples;
    BOOL _requireProxy, _noProbe, _openDetails, _snapshotExit, _snapshotStarted, _probeEnabled;
    NSString* _snapshotDirectory;
    NSTimer* _snapshotTimer;
    NSString* _lifecycleLogPath;
    FILE* _lifecycleLog;
    unsigned long long _sampleCount;
}
- (void)setLifecycleLogPath:(NSString*)path {
    _lifecycleLogPath=[path.stringByExpandingTildeInPath copy];
}
- (void)setTestHooksRequireProxy:(BOOL)requireProxy
                         noProbe:(BOOL)noProbe
                     openDetails:(BOOL)openDetails
                    snapshotExit:(BOOL)snapshotExit
                     snapshotDir:(NSString*)snapshotDir {
    _requireProxy=requireProxy;
    _noProbe=noProbe;
    _openDetails=openDetails;
    _snapshotExit=snapshotExit;
    NSString* expanded=snapshotDir.stringByExpandingTildeInPath;
    _snapshotDirectory=expanded.length ? [expanded copy] : nil;
}
- (BOOL)start {
    _trace=PowerTrace::Unknown; _colorIndex=-1;
    _startedAt=std::chrono::steady_clock::now();
    std::wstring error;
    if (!_settings.Load(error)) { Alert(Key::MacSettingsFailed,error); return NO; }
    if (_lifecycleLogPath.length) {
        const int fd=open(_lifecycleLogPath.fileSystemRepresentation,
            O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW,0600);
        if (fd>=0) {
            _lifecycleLog=fdopen(fd,"w");
            if (!_lifecycleLog) { const int saved=errno; close(fd); errno=saved; }
        }
        if (!_lifecycleLog) {
            std::fprintf(stderr,"Pulse: lifecycle log: %s\n",std::strerror(errno));
            return NO;
        }
    }
    i18n::SetLanguage(_settings.language);
    _item=[NSStatusBar.systemStatusBar statusItemWithLength:NSVariableStatusItemLength];
    _item.button.title=@""; _item.button.toolTip=ToNSString(T(Key::AppName));
    _statusView=[[PulseStatusView alloc] initWithFrame:_item.button.bounds];
    _statusView.autoresizingMask=NSViewWidthSizable | NSViewHeightSizable;
    [_item.button addSubview:_statusView];
    __weak PulseMenuBarController* weakSelf=self;
    _statusView.clicked=^(BOOL menu) {
        PulseMenuBarController* owner=weakSelf; if (!owner || owner->_stopping) return;
        if (menu) [owner showMenu];
        else [owner->_panels toggleDetailsFromButton:owner->_item.button components:owner->_settings.components];
    };
    _panels=[[PulsePanelController alloc] init];
    _panels.detailsChanged=^(BOOL visible) {
        (void)visible; PulseMenuBarController* owner=weakSelf;
        if (!owner || owner->_stopping) return;
        owner->_detailModel.BreakHistory(); owner->_forcePaint=YES;
        [owner updateProbeEnabled]; [owner sample]; [owner restartTimer];
    };
    _metrics=std::make_unique<Metrics>();
    if (_requireProxy) pulse::mac::SetProbeRequireProxy(true);
    [self updateProbeEnabled]; // Set before Start so a disabled probe never sends its first request.
    Settings network; network.slow_ms=_settings.slow_ms;
    _metrics->Start(network,ReceiveEvent,(__bridge void*)self);
    NSNotificationCenter* center=NSWorkspace.sharedWorkspace.notificationCenter;
    [center addObserver:self selector:@selector(willSleep:) name:NSWorkspaceWillSleepNotification object:nil];
    [center addObserver:self selector:@selector(didWake:) name:NSWorkspaceDidWakeNotification object:nil];
    [center addObserver:self selector:@selector(screensSleep:) name:NSWorkspaceScreensDidSleepNotification object:nil];
    [center addObserver:self selector:@selector(screensWake:) name:NSWorkspaceScreensDidWakeNotification object:nil];
    [self recordLifecycle:@"started"];
    _forcePaint=YES;
    if (_openDetails) {
        _panels.keepDetailsOpen=YES;
        [_panels toggleDetailsFromButton:_item.button components:_settings.components];
    } else [self sample];
    [self restartTimer]; return YES;
}
- (void)restartTimer {
    [_timer invalidate]; _timer=nil;
    if (_stopping || _screenAsleep || _systemAsleep) return;
    const unsigned interval=_panels.detailsVisible?1000u:_settings.refresh_ms;
    __weak PulseMenuBarController* weakSelf=self;
    _timer=[NSTimer timerWithTimeInterval:interval/1000.0 repeats:YES block:^(NSTimer* timer) { (void)timer; [weakSelf sample]; }];
    _timer.tolerance=MiniTimerTolerance(interval)/1000.0;
    [NSRunLoop.mainRunLoop addTimer:_timer forMode:NSRunLoopCommonModes];
}
- (void)sample {
    if (!_metrics || _stopping || _screenAsleep || _systemAsleep) return;
    const bool details=_panels.detailsVisible;
    auto menuComponents=_settings.components;
    menuComponents.taskbar.fill(false);
    menuComponents.taskbar[static_cast<std::size_t>(CompactComponent::Power)]=true;
    menuComponents.taskbar[static_cast<std::size_t>(CompactComponent::Eta)]=true;
    auto wants=CollectionWants(menuComponents,DisplayMode::Taskbar,details,_trace);
    // No other compact component consumes package watts. Full hardware always keeps IOReport.
    if (!details && CanSkipCompactPackagePower(_previousSnapshot)) wants.chip_power=false;
    // Unsupported domains must stay off even when SystemInfo implies cpu_freq.
    wants.cpu_freq=wants.fps=wants.gpu_temp=wants.gpu_memory=wants.wifi=wants.dgpu_power=false;
    _metrics->SetWants(wants);
    const auto snapshot=_metrics->Sample(details);
    _previousSnapshot=snapshot;
    _trace=NextCollectionTrace(_trace,snapshot.power_trace);
    const CGFloat width=[_statusView update:MakeCompactReadouts(snapshot)];
    if (std::fabs(_item.length-width)>.1) _item.length=width;
    _statusView.frame=_item.button.bounds;
    if (details) {
        const bool detailChanged=_detailModel.Update(snapshot,false,_settings.components,true);
        if (detailChanged || _forcePaint) [_panels updateDetails:_detailModel.Data() components:_settings.components];
    }
    _forcePaint=NO;
    ++_sampleCount;
    [self recordLifecycle:@"sample"];
    if (_snapshotDirectory.length) {
        ++_menuSamples;
        [self noteMenuSample];
    }
}
- (void)saveSettings {
    [_colorSaveTimer invalidate]; _colorSaveTimer=nil; _colorSavePending=NO;
    std::wstring error;
    if (!_settings.Save(error)) {
        if (_stopping) std::fprintf(stderr,"Pulse: %ls\n",error.c_str());
        else Alert(Key::MacSettingsFailed,error);
    }
}
- (void)updateProbeEnabled {
    if (!_metrics) return;
    const bool needed=!_noProbe && (_settings.notify ||
        (_panels.detailsVisible && _settings.components.Enabled(FullComponent::Network)));
    _probeEnabled=needed ? YES : NO;
    _metrics->SetProbeEnabled(needed);
}
- (void)settingsChanged {
    Sanitize(_settings.components);
    [self saveSettings];
    i18n::SetLanguage(_settings.language);
    _metrics->SetSlowThreshold(_settings.slow_ms);
    [self updateProbeEnabled];
    if (!_settings.notify) [_panels closeToast];
    [self recordLifecycle:@"settings-changed"];
    _forcePaint=YES; [self sample]; [self restartTimer];
}
- (void)handleEvent:(const NetEvent&)event {
    if (_stopping || _systemAsleep || _screenAsleep || !_settings.notify) return;
    [_panels showEvent:event fromButton:_item.button];
    if (_settings.sound) [[NSSound soundNamed:event.recovered?@"Glass":@"Basso"] play];
}
- (NSMenu*)buildMenu {
    NSMenu* root=[[NSMenu alloc] initWithTitle:@"Pulse"]; root.autoenablesItems=NO;
    NSMenu* show=Sub(root,Key::ShowItems);
    NSMenu* full=Sub(show,Key::MacDetails);
    for (auto c:FullOrder) {
        if (!Supported(c)) continue; const auto i=static_cast<std::size_t>(c);
        Checked(Item(full,T(FullComponents[i].label),self,@"toggleFull",i),_settings.components.full[i]);
    }
    [full addItem:NSMenuItem.separatorItem];
    Item(full,T(Key::SelectAll),self,@"allFull",1); Item(full,T(Key::SelectNone),self,@"allFull",0);
    Item(full,T(Key::ResetDefaults),self,@"defaultsFull");
    NSMenu* refresh=Sub(root,Key::RefreshInterval);
    const Key refreshLabels[]={Key::RefreshOnePlain,Key::RefreshTwo,Key::RefreshFive};
    for (std::size_t i=0;i<MiniRefreshIntervals.size();++i)
        Checked(Item(refresh,T(refreshLabels[i]),self,@"refresh",MiniRefreshIntervals[i]),_settings.refresh_ms==MiniRefreshIntervals[i]);
    NSMenu* slow=Sub(root,Key::SlowThreshold);
    for (int threshold:SlowThresholds) Checked(Item(slow,std::to_wstring(threshold)+T(Key::MsSuffix),self,@"slow",threshold),_settings.slow_ms==threshold);
    [root addItem:NSMenuItem.separatorItem];
    Checked(Item(root,T(Key::DisconnectAlerts),self,@"notify"),_settings.notify);
    Checked(Item(root,T(Key::AlertSound),self,@"sound"),_settings.sound);
    NSMenu* language=Sub(root,Key::LanguageMenu);
    const Key languageLabels[]={Key::LanguageAuto,Key::LanguageZh,Key::LanguageEn};
    for (NSInteger i=0;i<3;++i) Checked(Item(language,T(languageLabels[i]),self,@"language",i),static_cast<NSInteger>(_settings.language)==i);
    const auto status=SMAppService.mainAppService.status;
    NSMenuItem* login=Item(root,T(Key::MacOpenAtLogin),self,@"login");
    login.state=status==SMAppServiceStatusEnabled?NSControlStateValueOn:status==SMAppServiceStatusRequiresApproval?NSControlStateValueMixed:NSControlStateValueOff;
    [root addItem:NSMenuItem.separatorItem]; Item(root,T(Key::OpenLog),self,@"log");
    NSMenuItem* diagnostic=Item(root,T(Key::PowerDiagnostics),self,@"diagnose"); diagnostic.enabled=!_diagnosing;
    [root addItem:NSMenuItem.separatorItem]; Item(root,T(Key::Exit),self,@"exit");
    return root;
}
- (void)showMenu {
    [_panels closeDetails];
    NSMenu* root=[self buildMenu];
    // popUpMenuPositioningItem avoids deprecated NSStatusItem.popUpStatusItemMenu.
    // Location is the menu's top-left in view coordinates: just below the button either way.
    NSView* button=_item.button;
    const CGFloat below=button.isFlipped?NSMaxY(button.bounds)+2:NSMinY(button.bounds)-2;
    [root popUpMenuPositioningItem:nil atLocation:NSMakePoint(0,below) inView:button];
}
- (void)toggleLogin {
    SMAppService* service=SMAppService.mainAppService;
    const auto status=service.status;
    if (status==SMAppServiceStatusRequiresApproval) {
        Alert(Key::MacLoginApproval); [SMAppService openSystemSettingsLoginItems]; return;
    }
    NSError* error=nil;
    const BOOL removing=status==SMAppServiceStatusEnabled;
    const BOOL ok=removing?[service unregisterAndReturnError:&error]:[service registerAndReturnError:&error];
    if (!removing && service.status==SMAppServiceStatusRequiresApproval) {
        Alert(Key::MacLoginApproval); [SMAppService openSystemSettingsLoginItems];
    } else if (!ok) Alert(Key::MacLoginFailed,ToWide(error.localizedDescription));
}
- (void)chooseColor:(NSInteger)index {
    _colorIndex=index; NSColorPanel* panel=NSColorPanel.sharedColorPanel;
    panel.showsAlpha=NO; panel.continuous=YES;
    panel.color=_settings.colors[index]?Color(*_settings.colors[index]):NSColor.labelColor;
    panel.target=self; panel.action=@selector(colorChanged:);
    [NSApp activateIgnoringOtherApps:YES]; [panel makeKeyAndOrderFront:nil];
}
- (void)colorChanged:(id)sender {
    (void)sender; if (_stopping || _colorIndex<0 || static_cast<std::size_t>(_colorIndex)>=_settings.colors.size()) return;
    NSColor* color=[NSColorPanel.sharedColorPanel.color colorUsingColorSpace:NSColorSpace.sRGBColorSpace];
    if (!color) return;
    const auto channel=[](CGFloat x) { return static_cast<std::uint32_t>(std::lround(std::clamp<CGFloat>(x,0,1)*255)); };
    _settings.colors[_colorIndex]=(channel(color.redComponent)<<16)|(channel(color.greenComponent)<<8)|channel(color.blueComponent);
    // Dragging only updates the current paint; sampling and disk writes stay off this path.
    [_statusView setTextColors:_settings.colors];
    [_colorSaveTimer invalidate]; _colorSaveTimer=nil; _colorSavePending=YES;
    __weak PulseMenuBarController* weakSelf=self;
    _colorSaveTimer=[NSTimer timerWithTimeInterval:.5 repeats:NO block:^(NSTimer* timer) {
        (void)timer; PulseMenuBarController* owner=weakSelf;
        if (owner && !owner->_stopping) [owner saveSettings];
    }];
    [NSRunLoop.mainRunLoop addTimer:_colorSaveTimer forMode:NSRunLoopCommonModes];
}
- (void)diagnose {
    if (_diagnosing) return; _diagnosing=YES;
    __weak PulseMenuBarController* weakSelf=self;
    dispatch_async(dispatch_get_global_queue(QOS_CLASS_UTILITY,0),^{
        @autoreleasepool {
            std::wstring path,error; const int result=WritePowerDiagnosticsFile(&path,&error);
            dispatch_async(dispatch_get_main_queue(),^{
                PulseMenuBarController* owner=weakSelf;
                if (!owner || owner->_stopping) return;
                owner->_diagnosing=NO;
                if (result) Alert(Key::PowerDiagnosticsFailed,error);
                else if (![NSWorkspace.sharedWorkspace openURL:[NSURL fileURLWithPath:ToNSString(path)]]) Alert(Key::PowerDiagnosticsOpenFailed,path);
            });
        }
    });
}
- (void)menuAction:(NSMenuItem*)item {
    NSDictionary* data=item.representedObject; NSString* action=data[@"action"];
    const NSInteger value=[data[@"value"] integerValue],extra=[data[@"extra"] integerValue];
    const auto index=static_cast<std::size_t>(value);
    if ([action isEqualToString:@"exit"]) { [NSApp terminate:nil]; return; }
    if ([action isEqualToString:@"login"]) { [self toggleLogin]; return; }
    if ([action isEqualToString:@"log"]) {
        const auto file=_metrics->LogPath();
        if (file.empty()) Alert(Key::LogNotReady);
        else if (![NSWorkspace.sharedWorkspace openURL:[NSURL fileURLWithPath:ToNSString(file)]]) Alert(Key::LogOpenFailed);
        return;
    }
    if ([action isEqualToString:@"diagnose"]) { [self diagnose]; return; }
    if ([action isEqualToString:@"color"]) { [self chooseColor:value]; return; }
    if ([action isEqualToString:@"toggle"]) _settings.components.taskbar[index]=!_settings.components.taskbar[index];
    else if ([action isEqualToString:@"toggleFull"]) _settings.components.full[index]=!_settings.components.full[index];
    else if ([action isEqualToString:@"all"]) SelectComponents(_settings.components.taskbar,value!=0);
    else if ([action isEqualToString:@"allFull"]) SelectComponents(_settings.components.full,value!=0);
    else if ([action isEqualToString:@"defaults"]) {
        for (std::size_t i=0;i<_settings.components.taskbar.size();++i) _settings.components.taskbar[i]=TaskbarComponents[i].enabled;
        _settings.components.taskbarOrder=DefaultTaskbarOrder;
    } else if ([action isEqualToString:@"defaultsFull"]) {
        for (std::size_t i=0;i<_settings.components.full.size();++i) _settings.components.full[i]=FullComponents[i].enabled;
    } else if ([action isEqualToString:@"move"]) {
        const ComponentMove moves[]={ComponentMove::Up,ComponentMove::Down,ComponentMove::First,ComponentMove::Last};
        MoveComponent(_settings.components.taskbarOrder,_settings.components.taskbar,static_cast<CompactComponent>(value),moves[extra]);
    } else if ([action isEqualToString:@"orderDefault"]) _settings.components.taskbarOrder=DefaultTaskbarOrder;
    else if ([action isEqualToString:@"colorDefault"]) _settings.colors[index].reset();
    else if ([action isEqualToString:@"colorsDefault"]) for (auto& color:_settings.colors) color.reset();
    else if ([action isEqualToString:@"refresh"]) _settings.refresh_ms=ParseMiniRefresh(std::to_wstring(value));
    else if ([action isEqualToString:@"slow"]) _settings.slow_ms=static_cast<int>(value);
    else if ([action isEqualToString:@"notify"]) _settings.notify=!_settings.notify;
    else if ([action isEqualToString:@"sound"]) _settings.sound=!_settings.sound;
    else if ([action isEqualToString:@"language"]) _settings.language=static_cast<i18n::Preference>(value);
    [self settingsChanged];
}
- (void)willSleep:(NSNotification*)note {
    (void)note; if (_stopping) return; _systemAsleep=YES; [_timer invalidate]; _timer=nil;
    _metrics->OnSuspend(); [_panels closeDetails]; [_panels closeToast]; _detailModel.BreakHistory();
    [self recordLifecycle:@"will-sleep"];
}
- (void)didWake:(NSNotification*)note {
    (void)note; if (_stopping) return; _metrics->OnResume(); _systemAsleep=NO;
    _metrics->SetPaused(_screenAsleep);
    [self recordLifecycle:@"did-wake"];
    _forcePaint=YES; [self sample]; [self restartTimer];
}
- (void)screensSleep:(NSNotification*)note {
    (void)note; if (_stopping) return; _screenAsleep=YES; [_timer invalidate]; _timer=nil;
    _metrics->SetPaused(true); [_panels closeDetails]; [_panels closeToast]; _detailModel.BreakHistory();
    [self recordLifecycle:@"screens-sleep"];
}
- (void)screensWake:(NSNotification*)note {
    (void)note; if (_stopping) return; _screenAsleep=NO; _metrics->SetPaused(false);
    [self recordLifecycle:@"screens-wake"];
    _forcePaint=YES; [self sample]; [self restartTimer];
}
- (void)recordLifecycle:(NSString*)event {
    if (!_lifecycleLog) return;
    // Observe this process's existing callbacks and samples. No extra reads,
    // timers or network activity; normal launches never construct this payload.
    NSMutableDictionary* row=[@{
        @"event":event, @"unix_s":@(NSDate.date.timeIntervalSince1970),
        @"elapsed_s":@(std::chrono::duration<double>(std::chrono::steady_clock::now()-_startedAt).count()),
        @"pid":@(NSProcessInfo.processInfo.processIdentifier), @"sample_index":@(_sampleCount),
        @"system_asleep":@(_systemAsleep), @"screen_asleep":@(_screenAsleep),
        @"details_visible":@(_panels.detailsVisible), @"no_probe":@(_noProbe),
        @"probe_enabled":@(_probeEnabled), @"refresh_ms":@(_settings.refresh_ms),
        @"language":ToNSString(i18n::PreferenceValue(_settings.language)),
        @"power_chart_enabled":@(_settings.components.Enabled(FullComponent::PowerChart))
    } mutableCopy];
    if ([event isEqualToString:@"sample"]) {
        const auto number=[](double value)->id { return std::isfinite(value) ? (id)@(value) : NSNull.null; };
        const auto& sample=_previousSnapshot;
        const auto readouts=MakeCompactReadouts(sample);
        row[@"watts"]=ToNSString(readouts.watts); row[@"remaining"]=ToNSString(readouts.remaining);
        row[@"power_trace"]=ToNSString(PowerTraceName(sample.power_trace));
        row[@"power_mode"]=ToNSString(PowerModeName(sample.mode));
        row[@"battery_source"]=ToNSString(LastBatteryPowerSource());
        row[@"system_source"]=ToNSString(LastSystemPowerSource());
        row[@"ac_known"]=@(sample.ac_known); row[@"ac_online"]=@(sample.ac_online);
        row[@"power_w"]=number(sample.power_w); row[@"system_w"]=number(sample.system_w);
        row[@"battery_rate_mw"]=number(sample.battery_rate_mw);
        row[@"eta_s"]=number(sample.eta_s); row[@"on_battery_s"]=number(sample.on_battery_s);
        row[@"cpu_w"]=number(sample.cpu_w); row[@"pkg_w"]=number(sample.pkg_w);
        row[@"cpu_percent"]=number(sample.cpu_percent);
        row[@"down_bps"]=number(sample.down_bps); row[@"up_bps"]=number(sample.up_bps);
        row[@"net_state"]=ToNSString(NetStateName(sample.net_state));
        row[@"offline_since_unix"]=@(sample.offline_since_unix);
    }
    NSError* error=nil;
    NSData* bytes=[NSJSONSerialization dataWithJSONObject:row options:NSJSONWritingSortedKeys error:&error];
    if (!bytes || std::fwrite(bytes.bytes,1,bytes.length,_lifecycleLog)!=bytes.length ||
        std::fputc('\n',_lifecycleLog)==EOF || std::fflush(_lifecycleLog)!=0) {
        std::fprintf(stderr,"Pulse: lifecycle log write failed: %s\n",
            error ? error.localizedDescription.UTF8String : std::strerror(errno));
        std::fclose(_lifecycleLog); _lifecycleLog=nullptr;
    }
}
- (void)noteMenuSample {
    if (_stopping || _snapshotStarted || _snapshotDirectory.length==0) return;
    // First of: 3 completed menu-bar samples, or about 6 seconds after start.
    const double elapsed=std::chrono::duration<double>(std::chrono::steady_clock::now()-_startedAt).count();
    if (_menuSamples<3 && elapsed<6.0) return;
    _snapshotStarted=YES;
    [self beginSnapshot];
}
- (void)beginSnapshot {
    if (_stopping || _snapshotDirectory.length==0) return;
    NSError* writeError=nil;
    if (![NSFileManager.defaultManager createDirectoryAtPath:_snapshotDirectory withIntermediateDirectories:YES attributes:nil error:&writeError]) {
        [self reportURL:[NSURL fileURLWithPath:_snapshotDirectory isDirectory:YES] error:writeError fallback:@"could not create snapshot directory"];
        [self requestSnapshotExit];
        return;
    }
    // Hold the panel still until the bitmap is taken. --open-details keeps this on.
    _panels.keepDetailsOpen=YES;
    if (!_panels.detailsVisible)
        [_panels toggleDetailsFromButton:_item.button components:_settings.components];
    [self scheduleSnapshotFinish];
}
- (void)scheduleSnapshotFinish {
    [_snapshotTimer invalidate]; _snapshotTimer=nil;
    __weak PulseMenuBarController* weakSelf=self;
    _snapshotTimer=[NSTimer timerWithTimeInterval:1.0 repeats:NO block:^(NSTimer* fired) {
        (void)fired;
        PulseMenuBarController* owner=weakSelf;
        if (owner && !owner->_stopping) [owner finishSnapshot];
    }];
    [NSRunLoop.mainRunLoop addTimer:_snapshotTimer forMode:NSRunLoopCommonModes];
}
- (void)requestSnapshotExit {
    if (!_snapshotExit || _stopping) return;
    dispatch_async(dispatch_get_main_queue(), ^{ [NSApp terminate:nil]; });
}
- (void)reportURL:(NSURL*)url error:(NSError*)error fallback:(NSString*)fallback {
    NSString* path=url.path ?: @"(nil)";
    NSString* message=error.localizedDescription ?: fallback ?: @"unknown error";
    const char* pathBytes=path.UTF8String;
    const char* messageBytes=message.UTF8String;
    std::fprintf(stderr, "Pulse: %s: %s\n", pathBytes ? pathBytes : "(nil)", messageBytes ? messageBytes : "unknown error");
    std::fflush(stderr);
}
- (void)writePNG:(NSBitmapImageRep*)rep name:(NSString*)name {
    NSURL* url=[[NSURL fileURLWithPath:_snapshotDirectory isDirectory:YES] URLByAppendingPathComponent:name];
    NSError* writeError=nil;
    if (!rep || !PulseWritePNG(rep, url, &writeError))
        [self reportURL:url error:writeError fallback:@"bitmap failed"];
}
- (void)writeText:(NSString*)text name:(NSString*)name {
    NSURL* url=[[NSURL fileURLWithPath:_snapshotDirectory isDirectory:YES] URLByAppendingPathComponent:name];
    NSError* writeError=nil;
    if (!text || ![text writeToURL:url atomically:YES encoding:NSUTF8StringEncoding error:&writeError])
        [self reportURL:url error:writeError fallback:@"text write failed"];
}
- (NSString*)menuTreeText {
    NSMutableString* text=[NSMutableString string];
    AppendMenuItems([self buildMenu], 0, text);
    return text;
}
- (NSString*)snapshotStateTextWithBatterySource:(const wchar_t*)batterySource
                                   systemSource:(const wchar_t*)systemSource
                                       snapshot:(const Snapshot&)snapshot {
    NSMutableString* text=[NSMutableString string];
    auto add=[&](NSString* key, NSString* value) {
        [text appendFormat:@"%@=%@\n", key, value ?: @""];
    };
    auto addWide=[&](NSString* key, const std::wstring& value) {
        add(key, pulse::mac::ToNSString(value));
    };
    const double elapsed=std::chrono::duration<double>(std::chrono::steady_clock::now()-_startedAt).count();
    add(@"menu_samples", [NSString stringWithFormat:@"%d", _menuSamples]);
    add(@"elapsed_s", PulseNumber(elapsed));
    add(@"cpu_time_s", PulseNumber(pulse::mac::SelfCpuSeconds()));
    add(@"status_width_pt", _item ? PulseNumber(_item.length) : @"--");
    add(@"status_view_width_pt", PulseNumber(_statusView.bounds.size.width));
    add(@"status_view_height_pt", PulseNumber(_statusView.bounds.size.height));
    add(@"details_visible", YesNo(_panels.detailsVisible));
    add(@"probe_enabled", YesNo(_probeEnabled));
    add(@"require_proxy", YesNo(pulse::mac::ProbeRequireProxy()));
    add(@"no_probe", YesNo(_noProbe));
    add(@"open_details", YesNo(_openDetails));
    addWide(@"settings_file", _settings.file);
    add(@"refresh_ms", [NSString stringWithFormat:@"%u", _settings.refresh_ms]);
    addWide(@"language", std::wstring(i18n::CurrentLanguage()==i18n::Language::Chinese ? L"zh" : L"en"));
    addWide(@"language_preference", std::wstring(i18n::PreferenceValue(_settings.language)));
    addWide(@"power_trace", std::wstring(PowerTraceName(snapshot.power_trace)));
    add(@"power_w", PulseNumber(snapshot.power_w));
    add(@"system_w", PulseNumber(snapshot.system_w));
    add(@"battery_rate_mw", PulseNumber(snapshot.battery_rate_mw));
    add(@"battery_rate_valid", YesNo(snapshot.battery_rate_valid));
    add(@"ac_online", YesNo(snapshot.ac_online));
    add(@"ac_known", YesNo(snapshot.ac_known));
    add(@"battery_percent", snapshot.percent<0 ? @"--" : [NSString stringWithFormat:@"%d", snapshot.percent]);
    addWide(@"battery_power_source", std::wstring(batterySource ? batterySource : L""));
    addWide(@"system_power_source", std::wstring(systemSource ? systemSource : L""));
    const auto readouts=MakeCompactReadouts(snapshot);
    addWide(@"menubar.watts", readouts.watts);
    addWide(@"menubar.remaining", readouts.remaining);
    add(@"status_window_frame", NSStringFromRect(_item.button.window.frame));
    add(@"status_window_occlusion", [NSString stringWithFormat:@"%lu",static_cast<unsigned long>(_item.button.window.occlusionState)]);
    NSScreen* screen=_item.button.window.screen;
    add(@"screen_frame", NSStringFromRect(screen.frame));
    add(@"screen_safe_left", NSStringFromRect(screen.auxiliaryTopLeftArea));
    add(@"screen_safe_right", NSStringFromRect(screen.auxiliaryTopRightArea));
    const auto& detail=_detailModel.Data();
    addWide(@"detail.mode", std::wstring(PowerModeName(detail.mode)));
    addWide(@"detail.power_trace", std::wstring(PowerTraceName(detail.powerTrace)));
    addWide(@"detail.net_state", std::wstring(NetStateName(detail.netState)));
    addWide(@"detail.scheme", detail.scheme);
    addWide(@"detail.saver", detail.saverLabel);
    addWide(@"detail.percent", detail.percent);
    addWide(@"detail.hero", detail.heroWord);
    addWide(@"detail.watt_label", detail.wattLabel);
    addWide(@"detail.watt", detail.watt);
    addWide(@"detail.power_label", detail.powerLabel);
    addWide(@"detail.power_value", detail.powerValue);
    addWide(@"detail.package_w", detail.packageW);
    addWide(@"detail.package_note", detail.packageNote);
    addWide(@"detail.cpu_w", detail.cpuW);
    addWide(@"detail.gpu_w", detail.gpuW);
    addWide(@"detail.mem_w", detail.memW);
    addWide(@"detail.cpu", detail.cpuValue);
    addWide(@"detail.memory", detail.memoryValue);
    addWide(@"detail.latency", detail.latency);
    addWide(@"detail.net_hint", detail.netHint);
    return text;
}
- (void)finishSnapshot {
    [_snapshotTimer invalidate]; _snapshotTimer=nil;
    if (_stopping || _snapshotDirectory.length==0) return;
    [self updateProbeEnabled];
    [self sample];
    const wchar_t* batterySource=pulse::mac::LastBatteryPowerSource();
    const wchar_t* systemSource=pulse::mac::LastSystemPowerSource();
    const Snapshot shot=_previousSnapshot;
    [self writePNG:[_statusView bitmapWithAppearanceName:NSAppearanceNameDarkAqua fill:SnapshotFill(0x1e1e1e)] name:@"menubar-dark.png"];
    [self writePNG:[_statusView bitmapWithAppearanceName:NSAppearanceNameAqua fill:SnapshotFill(0xf0f0f0)] name:@"menubar-light.png"];
    NSURL* directory=[NSURL fileURLWithPath:_snapshotDirectory isDirectory:YES];
    NSError* writeError=nil;
    NSURL* detailsURL=[directory URLByAppendingPathComponent:@"details.png"];
    if (![_panels writeDetailsPNGToURL:detailsURL error:&writeError])
        [self reportURL:detailsURL error:writeError fallback:@"details bitmap failed"];
    const auto unixNow=static_cast<std::int64_t>(std::time(nullptr));
    pulse::NetEvent offline{};
    offline.recovered=false;
    offline.reason=pulse::OfflineReason::Timeout;
    offline.offline_start_unix=unixNow;
    offline.event_unix=unixNow;
    pulse::NetEvent recovered{};
    recovered.recovered=true;
    recovered.outage_duration_s=35;
    recovered.event_unix=unixNow;
    recovered.offline_start_unix=unixNow>=35 ? unixNow-35 : 0;
    writeError=nil;
    NSURL* offlineURL=[directory URLByAppendingPathComponent:@"toast-offline.png"];
    if (![_panels writeToastPNGToURL:offlineURL netEvent:offline error:&writeError])
        [self reportURL:offlineURL error:writeError fallback:@"toast bitmap failed"];
    writeError=nil;
    NSURL* recoveredURL=[directory URLByAppendingPathComponent:@"toast-recovered.png"];
    if (![_panels writeToastPNGToURL:recoveredURL netEvent:recovered error:&writeError])
        [self reportURL:recoveredURL error:writeError fallback:@"toast bitmap failed"];
    [self writeText:[self menuTreeText] name:@"menu.txt"];
    [self writeText:[self snapshotStateTextWithBatterySource:batterySource systemSource:systemSource snapshot:shot] name:@"state.txt"];
    if (!_openDetails) {
        _panels.keepDetailsOpen=NO;
        [_panels closeDetails];
    }
    [self requestSnapshotExit];
}
- (void)shutdown {
    if (_stopping) return; _stopping=YES;
    [self recordLifecycle:@"shutdown"];
    if (_lifecycleLog) { std::fclose(_lifecycleLog); _lifecycleLog=nullptr; }
    [_timer invalidate]; _timer=nil;
    [_snapshotTimer invalidate]; _snapshotTimer=nil;
    if (_colorSavePending) [self saveSettings];
    [_colorSaveTimer invalidate]; _colorSaveTimer=nil;
    [NSWorkspace.sharedWorkspace.notificationCenter removeObserver:self];
    [_panels shutdown]; _statusView.clicked=nil;
    NSColorPanel* color=NSColorPanel.sharedColorPanel;
    if (_colorIndex>=0) { color.target=nil; color.action=nil; [color orderOut:nil]; }
    _metrics.reset(); // Stop/join callback producer before this controller is released.
    if (_item) [NSStatusBar.systemStatusBar removeStatusItem:_item]; _item=nil;
}
- (void)dealloc { [self shutdown]; }
@end
