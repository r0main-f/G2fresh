#include "NoAppNap.h"

#import <Foundation/Foundation.h>

// Explicit retain and release (CFBridgingRetain / CFBridgingRelease): correct with or without ARC.
NoAppNap::NoAppNap()
{
    id token = [[NSProcessInfo processInfo]
        beginActivityWithOptions:NSActivityUserInitiated | NSActivityLatencyCritical
                          reason:@"G2fresh: the emulated G2 runs in real time"];
    activity_ = const_cast<void*>(CFBridgingRetain(token));
}

NoAppNap::~NoAppNap()
{
    if (activity_ != nullptr)
        [[NSProcessInfo processInfo] endActivity:CFBridgingRelease(activity_)];
}
