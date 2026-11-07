// Exercise the production status view without starting Metrics or a status item.
#include "menubar_mac.mm"
#include <cassert>

int main(int argc, const char* argv[]) {
    @autoreleasepool {
        assert(argc==2);
        [NSApplication.sharedApplication setActivationPolicy:NSApplicationActivationPolicyAccessory];
        NSString* directory=[NSString stringWithUTF8String:argv[1]];
        [NSFileManager.defaultManager createDirectoryAtPath:directory withIntermediateDirectories:YES attributes:nil error:nil];
        PulseStatusView* view=[[PulseStatusView alloc] initWithFrame:NSMakeRect(0,0,1,22)];
        NSFont* font=[NSFont monospacedDigitSystemFontOfSize:12 weight:NSFontWeightRegular];
        const CompactReadouts cases[]={{L"5.2W",L"6:30"},{L"--W",L"--:--"},
            {L"5.2W",L"0:45"},{L"123.4W",L"100:00"},{L"9999.9W",L"277777:46"},
            {L"5.2W",L"6:30"}};
        unsigned index=0;
        for (const auto& value:cases) {
            const CGFloat width=[view update:value];
            assert(width==10+TextWidth(value.watts,font)+TextWidth(value.remaining,font));
            view.frame=NSMakeRect(0,0,width,22);
            std::printf("%u %ls %ls width=%.0fpt height=22pt font=12pt\n",index,value.watts.c_str(),value.remaining.c_str(),width);
            for (bool dark:{false,true}) {
                NSString* name=[NSString stringWithFormat:@"case-%u-%@.png",index,dark?@"dark":@"light"];
                auto bitmap=[view bitmapWithAppearanceName:dark?NSAppearanceNameDarkAqua:NSAppearanceNameAqua fill:SnapshotFill(dark?0x1e1e1e:0xf0f0f0)];
                NSError* error=nil;
                assert(PulseWritePNG(bitmap,[NSURL fileURLWithPath:[directory stringByAppendingPathComponent:name]],&error));
            }
            ++index;
        }
        std::puts("6 production-render width cases / 12 images passed; no sampler or network started");
    }
}
