#import <Foundation/Foundation.h>
#include "../src/i18n.h"
namespace pulse::i18n {
bool SystemPrefersChinese() {
    NSString* language = NSLocale.preferredLanguages.firstObject;
    return language && [language hasPrefix:@"zh"];
}
}
