#include "include/ISS.h"

#include <ApplicationServices/ApplicationServices.h>
#include <CoreFoundation/CoreFoundation.h>
#include <CoreGraphics/CGEventTypes.h>
#include <assert.h>
#include <dlfcn.h>
#include <float.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

static const CGEventField kCGSEventTypeField = (CGEventField)55;
static const CGEventField kCGEventGestureHIDType = (CGEventField)110;
static const CGEventField kCGEventGestureSwipeMotion = (CGEventField)123;
static const CGEventField kCGEventGestureSwipeProgress = (CGEventField)124;
static const CGEventField kCGEventGestureSwipeVelocityX = (CGEventField)129;
static const CGEventField kCGEventGestureSwipeVelocityY = (CGEventField)130;
static const CGEventField kCGEventGesturePhase = (CGEventField)132;

static const int64_t kVKLeftArrow = 123;
static const int64_t kVKRightArrow = 124;

// See IOHIDEventType enum in IOHIDFamily
static const uint32_t kIOHIDEventTypeDockSwipe = 23;

typedef uint32_t CGSEventType;
enum {
    kCGSEventScrollWheel = 22,
    kCGSEventZoom = 28,
    kCGSEventGesture = 29,
    kCGSEventDockControl = 30,
    kCGSEventFluidTouchGesture = 31,
};

typedef CF_ENUM(uint8_t, CGSGesturePhase) {
    kCGSGesturePhaseNone = 0,
    kCGSGesturePhaseBegan = 1,
    kCGSGesturePhaseChanged = 2,
    kCGSGesturePhaseEnded = 4,
    kCGSGesturePhaseCancelled = 8,
    kCGSGesturePhaseMayBegin = 128,
};

// Limited subset of motion constants observed in synthetic Dock swipe traces.
typedef CF_ENUM(uint16_t, CGGestureMotion) {
    kCGGestureMotionHorizontal = 1,
};

typedef int32_t CGSConnectionID;
typedef uint64_t CGSSpaceID;

extern CFArrayRef CGSCopyManagedDisplaySpaces(CGSConnectionID connection, CFStringRef display) __attribute__((weak_import));
extern CFStringRef CGSCopyActiveMenuBarDisplayIdentifier(CGSConnectionID connection) __attribute__((weak_import));
extern CGSConnectionID CGSMainConnectionID(void) __attribute__((weak_import));
extern CGSSpaceID CGSGetActiveSpace(CGSConnectionID connection) __attribute__((weak_import));

static CFMachPortRef globalTap = NULL;
static CFRunLoopSourceRef globalSource = NULL;

// Overlay detection state
static bool overlayDetectionEnabled = false;

// Swipe override state
static bool swipeOverrideEnabled = false;
static bool swipeTracking = false;
static bool swipeFired = false;
static bool suppressSwitchShortcutKeyUp = false;

// Gesture speed state
static double gestureSpeed = 2000.0;

static ISSSwitchCallback switchCallback = NULL;

// Predictions dictionary: DisplayID (CFStringRef) -> Index (CFNumberRef)
static CFMutableDictionaryRef predictionsDict = NULL;

static bool get_prediction(const char *displayID, unsigned int *outIndex) {
    if (!displayID || !predictionsDict) return false;
    
    CFStringRef key = CFStringCreateWithCString(NULL, displayID, kCFStringEncodingUTF8);
    const void *value = CFDictionaryGetValue(predictionsDict, key);
    CFRelease(key);

    if (value) {
        CFNumberGetValue((CFNumberRef)value, kCFNumberIntType, outIndex);
        return true;
    }
    return false;
}

static void set_prediction(const char *displayID, unsigned int index) {
    if (!displayID || !predictionsDict) return;
    
    CFStringRef key = CFStringCreateWithCString(NULL, displayID, kCFStringEncodingUTF8);
    CFNumberRef val = CFNumberCreate(NULL, kCFNumberIntType, &index);
    CFDictionarySetValue(predictionsDict, key, val);
    CFRelease(key);
    CFRelease(val);
}

static bool extract_space_info_from_display(CFDictionaryRef displayDict,
                                            CGSSpaceID activeSpace,
                                            bool hasActiveSpace,
                                            ISSSpaceInfo *outInfo);
static CFDictionaryRef copy_target_display(bool useCursorDisplay,
                                           CGSSpaceID *activeSpace,
                                           bool *hasActiveSpace);
static bool load_space_info_for_display(ISSSpaceInfo *info, bool useCursorDisplay);
static bool iss_perform_switch_gesture(ISSDirection direction, double velocity);
static bool iss_switch_with_info(const ISSSpaceInfo *info, ISSDirection direction);
static bool iss_should_block_switch(const ISSSpaceInfo *info, ISSDirection direction);

// Perform a swipe-override switch: get space info, compute target, switch,
// and notify the handler with the target index.
static void swipe_override_switch(ISSDirection dir) {
    ISSSpaceInfo info;
    if (!iss_get_space_info(&info)) {
        iss_perform_switch_gesture(dir, gestureSpeed);
        return;
    }

    unsigned int predicted;
    unsigned int current = get_prediction(info.displayID, &predicted) ? predicted : info.currentIndex;
    unsigned int target = dir == ISSDirectionLeft ? current - 1 : current + 1;

    if (iss_switch_with_info(&info, dir)) {
        set_prediction(info.displayID, target);
        if (switchCallback) { switchCallback(target); }
    }
}

static bool is_control_space_switch_shortcut(CGEventRef event, ISSDirection *outDirection) {
    if (!event || !outDirection) return false;

    int64_t keyCode = CGEventGetIntegerValueField(event, kCGKeyboardEventKeycode);
    if (keyCode != kVKLeftArrow && keyCode != kVKRightArrow) {
        return false;
    }

    CGEventFlags flags = CGEventGetFlags(event);
    const CGEventFlags required = kCGEventFlagMaskControl;
    const CGEventFlags disallowed = kCGEventFlagMaskAlternate |
                                    kCGEventFlagMaskCommand |
                                    kCGEventFlagMaskShift;

    if ((flags & required) == 0 || (flags & disallowed) != 0) {
        return false;
    }

    *outDirection = keyCode == kVKLeftArrow ? ISSDirectionLeft : ISSDirectionRight;
    return true;
}

static CGEventRef eventTapCallback(CGEventTapProxy proxy, CGEventType type,
                                   CGEventRef event, void *refcon) {
    (void)proxy;
    (void)refcon;

    // Re-enable if the system disabled our tap for being too slow
    if (type == kCGEventTapDisabledByTimeout || type == kCGEventTapDisabledByUserInput) {
        if (globalTap) CGEventTapEnable(globalTap, true);
        return event;
    }

    if (!swipeOverrideEnabled) return event;

    if (type == kCGEventKeyDown) {
        ISSDirection direction;
        if (is_control_space_switch_shortcut(event, &direction)) {
            suppressSwitchShortcutKeyUp = true;
            swipe_override_switch(direction);
            return NULL;
        }
    }

    if (type == kCGEventKeyUp && suppressSwitchShortcutKeyUp) {
        ISSDirection direction;
        if (is_control_space_switch_shortcut(event, &direction)) {
            suppressSwitchShortcutKeyUp = false;
            return NULL;
        }
        suppressSwitchShortcutKeyUp = false;
    }

    CGSEventType eventType =
        (CGSEventType)CGEventGetIntegerValueField(event, kCGSEventTypeField);

    // Pass through synthetic events (non-HID source). Real Dock swipe events
    // from the trackpad have sourcePid == 0 (HID kernel). Magic Mouse fluid
    // touch gestures may have a nonzero source pid, so handle those below.
    if (eventType == kCGSEventDockControl || eventType == kCGSEventGesture) {
        pid_t sourcePid = (pid_t)CGEventGetIntegerValueField(event, kCGEventSourceUnixProcessID);
        if (sourcePid != 0) return event;
    }

    if (eventType == kCGSEventDockControl || eventType == kCGSEventFluidTouchGesture) {
        if (eventType == kCGSEventDockControl) {
            uint32_t hidType =
                (uint32_t)CGEventGetIntegerValueField(event, kCGEventGestureHIDType);
            if (hidType != kIOHIDEventTypeDockSwipe) return event;
        }

        if (eventType == kCGSEventDockControl) {
            uint16_t motion =
                (uint16_t)CGEventGetIntegerValueField(event, kCGEventGestureSwipeMotion);
            if (motion != kCGGestureMotionHorizontal) return event;
        }

        CGSGesturePhase phase =
            (CGSGesturePhase)CGEventGetIntegerValueField(event, kCGEventGesturePhase);

        switch (phase) {
        case kCGSGesturePhaseBegan:
            if (iss_is_expose_active()) return event;
            swipeTracking = true;
            swipeFired = false;
            return NULL;

        case kCGSGesturePhaseChanged: {
            if (!swipeTracking) return event;
            if (!swipeFired) {
                double progress =
                    CGEventGetDoubleValueField(event, kCGEventGestureSwipeProgress);
                if (progress != 0.0) {
                    ISSDirection dir =
                        progress > 0 ? ISSDirectionRight : ISSDirectionLeft;
                    swipeFired = true;
                    swipe_override_switch(dir);
                }
            }
            return NULL;
        }

        case kCGSGesturePhaseEnded: {
            if (!swipeTracking) return event;
            if (!swipeFired) {
                double velocity =
                    CGEventGetDoubleValueField(event, kCGEventGestureSwipeVelocityX);
                if (velocity != 0.0) {
                    ISSDirection dir =
                        velocity > 0 ? ISSDirectionRight : ISSDirectionLeft;
                    swipeFired = true;
                    swipe_override_switch(dir);
                }
            }
            swipeTracking = false;
            swipeFired = false;
            return NULL;
        }

        case kCGSGesturePhaseCancelled:
            swipeTracking = false;
            swipeFired = false;
            return NULL;

        default:
            return swipeTracking ? NULL : event;
        }
    }

    // Suppress companion gesture events during active swipe tracking
    if ((eventType == kCGSEventGesture || eventType == kCGSEventFluidTouchGesture) && swipeTracking) {
        return NULL;
    }

    return event;
}

static bool cgs_symbols_available(void) {
    return (&CGSMainConnectionID != NULL) &&
           (&CGSGetActiveSpace != NULL) &&
           (&CGSCopyManagedDisplaySpaces != NULL);
}

static bool extract_space_info_from_display(CFDictionaryRef displayDict,
                                            CGSSpaceID activeSpace,
                                            bool hasActiveSpace,
                                            ISSSpaceInfo *outInfo) {
    if (!displayDict || !outInfo) {
        return false;
    }

    memset(outInfo->displayID, 0, sizeof(outInfo->displayID));
    CFStringRef identifier = (CFStringRef)CFDictionaryGetValue(displayDict, CFSTR("Display Identifier"));
    if (identifier && CFGetTypeID(identifier) == CFStringGetTypeID()) {
        CFStringGetCString(identifier, outInfo->displayID, sizeof(outInfo->displayID), kCFStringEncodingUTF8);
    }

    const void *spacesValue = CFDictionaryGetValue(displayDict, CFSTR("Spaces"));
    if (!spacesValue || CFGetTypeID(spacesValue) != CFArrayGetTypeID()) {
        return false;
    }

    // Try to get current space from display dict (more accurate per-display)
    CGSSpaceID displayActiveSpace = 0;
    const void *currentSpaceValue = CFDictionaryGetValue(displayDict, CFSTR("Current Space"));
    if (currentSpaceValue && CFGetTypeID(currentSpaceValue) == CFDictionaryGetTypeID()) {
        CFDictionaryRef currentSpaceDict = (CFDictionaryRef)currentSpaceValue;
        CFNumberRef currentSpaceID = (CFNumberRef)CFDictionaryGetValue(currentSpaceDict, CFSTR("id64"));
        if (currentSpaceID && CFGetTypeID(currentSpaceID) == CFNumberGetTypeID()) {
            CFNumberGetValue(currentSpaceID, kCFNumberSInt64Type, &displayActiveSpace);
        }
    }
    
    // Use display-specific active space if available, otherwise use global
    CGSSpaceID targetActiveSpace = displayActiveSpace != 0 ? displayActiveSpace : activeSpace;
    bool hasTargetActiveSpace = displayActiveSpace != 0 || hasActiveSpace;

    CFArrayRef spaces = (CFArrayRef)spacesValue;
    const CFIndex spaceCount = CFArrayGetCount(spaces);

    unsigned int totalSpaces = 0;
    unsigned int activeIndex = 0;
    bool foundActive = false;

    for (CFIndex i = 0; i < spaceCount; i++) {
        const void *spaceValue = CFArrayGetValueAtIndex(spaces, i);
        if (!spaceValue || CFGetTypeID(spaceValue) != CFDictionaryGetTypeID()) {
            continue;
        }

        CFDictionaryRef spaceDict = (CFDictionaryRef)spaceValue;
        CFNumberRef idNumber = (CFNumberRef)CFDictionaryGetValue(spaceDict, CFSTR("id64"));
        if (!idNumber || CFGetTypeID(idNumber) != CFNumberGetTypeID()) {
            continue;
        }

        CGSSpaceID candidate = 0;
        if (CFNumberGetValue(idNumber, kCFNumberSInt64Type, &candidate)) {
            if (!foundActive && hasTargetActiveSpace && candidate == targetActiveSpace) {
                activeIndex = totalSpaces;
                foundActive = true;
            }
            totalSpaces++;
        }
    }

    if (totalSpaces == 0 || (hasTargetActiveSpace && !foundActive)) {
        return false;
    }

    outInfo->spaceCount = totalSpaces;
    outInfo->currentIndex = foundActive ? activeIndex : 0;
    return true;
}

static CFDictionaryRef copy_target_display(bool useCursorDisplay,
                                           CGSSpaceID *activeSpace,
                                           bool *hasActiveSpace) {
    if (!cgs_symbols_available()) {
        fprintf(stderr, "ISS: required CGS symbols missing\n");
        return NULL;
    }

    CGSConnectionID connection = CGSMainConnectionID();
    if (connection == 0) {
        fprintf(stderr, "ISS: CGSMainConnectionID returned 0\n");
        return NULL;
    }

    CGSSpaceID localActiveSpace = 0;
    bool localHasActiveSpace = false;
    if (&CGSGetActiveSpace != NULL) {
        localActiveSpace = CGSGetActiveSpace(connection);
        if (localActiveSpace != 0) {
            localHasActiveSpace = true;
        } else {
            fprintf(stderr, "ISS: CGSGetActiveSpace returned 0\n");
            return NULL;
        }
    }

    if (activeSpace) {
        *activeSpace = localActiveSpace;
    }
    if (hasActiveSpace) {
        *hasActiveSpace = localHasActiveSpace;
    }

    // Get display identifier based on mode
    CFStringRef activeDisplayIdentifier = NULL;
    
    if (useCursorDisplay) {
        // Get display where cursor is located
        CGEventRef tempEvent = CGEventCreate(NULL);
        CGPoint cursorLocation = CGEventGetLocation(tempEvent);
        CFRelease(tempEvent);
        
        CGDirectDisplayID cursorDisplay = 0;
        uint32_t cursorDisplayCount = 0;
        
        if (CGGetDisplaysWithPoint(cursorLocation, 1, &cursorDisplay, &cursorDisplayCount) == kCGErrorSuccess && cursorDisplayCount > 0) {
            CFUUIDRef displayUUID = CGDisplayCreateUUIDFromDisplayID(cursorDisplay);
            if (displayUUID) {
                activeDisplayIdentifier = CFUUIDCreateString(NULL, displayUUID);
                CFRelease(displayUUID);
            }
        }
    } else {
        // Get menubar display
        if (&CGSCopyActiveMenuBarDisplayIdentifier != NULL) {
            activeDisplayIdentifier = CGSCopyActiveMenuBarDisplayIdentifier(connection);
        }
    }

    CFArrayRef displays = CGSCopyManagedDisplaySpaces(connection, activeDisplayIdentifier);
    if (!displays && activeDisplayIdentifier) {
        displays = CGSCopyManagedDisplaySpaces(connection, NULL);
    }
    if (!displays) {
        if (activeDisplayIdentifier) {
            CFRelease(activeDisplayIdentifier);
        }
        return NULL;
    }

    const CFIndex displayCount = CFArrayGetCount(displays);
    CFDictionaryRef targetDisplay = NULL;
    CFDictionaryRef fallbackDisplay = NULL;

    for (CFIndex i = 0; i < displayCount; i++) {
        const void *displayValue = CFArrayGetValueAtIndex(displays, i);
        if (!displayValue || CFGetTypeID(displayValue) != CFDictionaryGetTypeID()) {
            continue;
        }

        CFDictionaryRef displayDict = (CFDictionaryRef)displayValue;

        if (!fallbackDisplay) {
            fallbackDisplay = displayDict;
        }

        if (!activeDisplayIdentifier || targetDisplay) {
            continue;
        }

        CFStringRef identifier = (CFStringRef)CFDictionaryGetValue(displayDict, CFSTR("Display Identifier"));
        if (identifier && CFGetTypeID(identifier) == CFStringGetTypeID() && CFEqual(identifier, activeDisplayIdentifier)) {
            targetDisplay = displayDict;
        }
    }

    if (!targetDisplay) {
        targetDisplay = fallbackDisplay;
    }

    CFDictionaryRef copiedDisplay = targetDisplay ? CFRetain(targetDisplay) : NULL;

    if (activeDisplayIdentifier) {
        CFRelease(activeDisplayIdentifier);
    }
    CFRelease(displays);

    return copiedDisplay;
}

static bool load_space_info_for_display(ISSSpaceInfo *info, bool useCursorDisplay) {
    CGSSpaceID activeSpace = 0;
    bool hasActiveSpace = false;
    CFDictionaryRef targetDisplay = copy_target_display(useCursorDisplay, &activeSpace, &hasActiveSpace);
    if (!targetDisplay) {
        return false;
    }

    bool success = extract_space_info_from_display(targetDisplay, activeSpace, hasActiveSpace, info);
    CFRelease(targetDisplay);
    return success;
}

static bool space_dict_has_window_id(CFDictionaryRef spaceDict, int windowID) {
    if (!spaceDict) return false;

    const CFStringRef keys[] = {
        CFSTR("windows"),
        CFSTR("Windows"),
        CFSTR("Window IDs"),
        CFSTR("ManagedSpaceWindows")
    };

    for (size_t keyIndex = 0; keyIndex < sizeof(keys) / sizeof(keys[0]); keyIndex++) {
        const void *value = CFDictionaryGetValue(spaceDict, keys[keyIndex]);
        if (!value || CFGetTypeID(value) != CFArrayGetTypeID()) {
            continue;
        }

        CFArrayRef windows = (CFArrayRef)value;
        CFIndex count = CFArrayGetCount(windows);
        for (CFIndex i = 0; i < count; i++) {
            const void *windowValue = CFArrayGetValueAtIndex(windows, i);
            if (!windowValue || CFGetTypeID(windowValue) != CFNumberGetTypeID()) {
                continue;
            }

            int candidate = 0;
            if (CFNumberGetValue((CFNumberRef)windowValue, kCFNumberIntType, &candidate) && candidate == windowID) {
                return true;
            }
        }
    }

    return false;
}

static bool owner_is_ignored(CFStringRef owner) {
    if (!owner) return true;
    return CFEqual(owner, CFSTR("Dock")) ||
           CFEqual(owner, CFSTR("WindowServer")) ||
           CFEqual(owner, CFSTR("SystemUIServer")) ||
           CFEqual(owner, CFSTR("Control Center")) ||
           CFEqual(owner, CFSTR("NotificationCenter"));
}

static CFStringRef copy_owner_name_for_pid(CFArrayRef windowList, int pid) {
    if (!windowList || pid <= 0) return NULL;

    CFIndex count = CFArrayGetCount(windowList);
    for (CFIndex i = 0; i < count; i++) {
        const void *windowValue = CFArrayGetValueAtIndex(windowList, i);
        if (!windowValue || CFGetTypeID(windowValue) != CFDictionaryGetTypeID()) {
            continue;
        }

        CFDictionaryRef windowInfo = (CFDictionaryRef)windowValue;
        CFNumberRef pidNumber = (CFNumberRef)CFDictionaryGetValue(windowInfo, CFSTR("kCGWindowOwnerPID"));
        int candidatePID = 0;
        if (!pidNumber || CFGetTypeID(pidNumber) != CFNumberGetTypeID() ||
            !CFNumberGetValue(pidNumber, kCFNumberIntType, &candidatePID) ||
            candidatePID != pid) {
            continue;
        }

        CFStringRef owner = (CFStringRef)CFDictionaryGetValue(windowInfo, CFSTR("kCGWindowOwnerName"));
        if (!owner || CFGetTypeID(owner) != CFStringGetTypeID() || owner_is_ignored(owner)) {
            continue;
        }

        return CFRetain(owner);
    }

    char fallback[64];
    snprintf(fallback, sizeof(fallback), "PID %d", pid);
    return CFStringCreateWithCString(NULL, fallback, kCFStringEncodingUTF8);
}

bool iss_primary_owner_for_space_window_list(CFDictionaryRef spaceDict,
                                             CFArrayRef windowList,
                                             int *outPID,
                                             CFStringRef *outOwnerName) {
    if (outPID) *outPID = 0;
    if (outOwnerName) *outOwnerName = NULL;
    if (!spaceDict || !windowList) return false;

    CFIndex count = CFArrayGetCount(windowList);
    for (CFIndex i = 0; i < count; i++) {
        const void *windowValue = CFArrayGetValueAtIndex(windowList, i);
        if (!windowValue || CFGetTypeID(windowValue) != CFDictionaryGetTypeID()) {
            continue;
        }

        CFDictionaryRef windowInfo = (CFDictionaryRef)windowValue;
        CFNumberRef windowNumber = (CFNumberRef)CFDictionaryGetValue(windowInfo, CFSTR("kCGWindowNumber"));
        if (!windowNumber || CFGetTypeID(windowNumber) != CFNumberGetTypeID()) {
            continue;
        }

        int windowID = 0;
        if (!CFNumberGetValue(windowNumber, kCFNumberIntType, &windowID) ||
            !space_dict_has_window_id(spaceDict, windowID)) {
            continue;
        }

        CFStringRef owner = (CFStringRef)CFDictionaryGetValue(windowInfo, CFSTR("kCGWindowOwnerName"));
        if (!owner || CFGetTypeID(owner) != CFStringGetTypeID() || owner_is_ignored(owner)) {
            continue;
        }

        int pid = 0;
        CFNumberRef pidNumber = (CFNumberRef)CFDictionaryGetValue(windowInfo, CFSTR("kCGWindowOwnerPID"));
        if (pidNumber && CFGetTypeID(pidNumber) == CFNumberGetTypeID()) {
            CFNumberGetValue(pidNumber, kCFNumberIntType, &pid);
        }

        if (outPID) *outPID = pid;
        if (outOwnerName) *outOwnerName = CFRetain(owner);
        return true;
    }

    return false;
}

static void append_pid_if_missing(CFMutableArrayRef pids, int pid) {
    if (!pids || pid <= 0) return;

    CFIndex count = CFArrayGetCount(pids);
    for (CFIndex i = 0; i < count; i++) {
        CFNumberRef number = (CFNumberRef)CFArrayGetValueAtIndex(pids, i);
        int candidate = 0;
        if (number && CFGetTypeID(number) == CFNumberGetTypeID() &&
            CFNumberGetValue(number, kCFNumberIntType, &candidate) &&
            candidate == pid) {
            return;
        }
    }

    CFNumberRef number = CFNumberCreate(NULL, kCFNumberIntType, &pid);
    CFArrayAppendValue(pids, number);
    CFRelease(number);
}

static void append_pids_from_value(CFMutableArrayRef pids, const void *value) {
    if (!pids || !value) return;

    if (CFGetTypeID(value) == CFNumberGetTypeID()) {
        int pid = 0;
        if (CFNumberGetValue((CFNumberRef)value, kCFNumberIntType, &pid)) {
            append_pid_if_missing(pids, pid);
        }
        return;
    }

    if (CFGetTypeID(value) == CFArrayGetTypeID()) {
        CFArrayRef array = (CFArrayRef)value;
        CFIndex count = CFArrayGetCount(array);
        for (CFIndex i = 0; i < count; i++) {
            append_pids_from_value(pids, CFArrayGetValueAtIndex(array, i));
        }
    }
}

static CFArrayRef copy_pids_for_space(CFDictionaryRef spaceDict) {
    if (!spaceDict) return NULL;

    CFMutableArrayRef pids = CFArrayCreateMutable(NULL, 0, &kCFTypeArrayCallBacks);
    append_pids_from_value(pids, CFDictionaryGetValue(spaceDict, CFSTR("pid")));

    const void *layoutValue = CFDictionaryGetValue(spaceDict, CFSTR("TileLayoutManager"));
    if (layoutValue && CFGetTypeID(layoutValue) == CFDictionaryGetTypeID()) {
        CFDictionaryRef layout = (CFDictionaryRef)layoutValue;
        const void *tileSpacesValue = CFDictionaryGetValue(layout, CFSTR("TileSpaces"));
        if (tileSpacesValue && CFGetTypeID(tileSpacesValue) == CFArrayGetTypeID()) {
            CFArrayRef tileSpaces = (CFArrayRef)tileSpacesValue;
            CFIndex count = CFArrayGetCount(tileSpaces);
            for (CFIndex i = 0; i < count; i++) {
                const void *tileValue = CFArrayGetValueAtIndex(tileSpaces, i);
                if (tileValue && CFGetTypeID(tileValue) == CFDictionaryGetTypeID()) {
                    append_pids_from_value(pids, CFDictionaryGetValue((CFDictionaryRef)tileValue, CFSTR("pid")));
                }
            }
        }
    }

    if (CFArrayGetCount(pids) == 0) {
        CFRelease(pids);
        return NULL;
    }

    return pids;
}

static bool space_dict_is_fullscreen(CFDictionaryRef spaceDict) {
    if (!spaceDict) return false;

    CFNumberRef typeNumber = (CFNumberRef)CFDictionaryGetValue(spaceDict, CFSTR("type"));
    int type = 0;
    if (typeNumber && CFGetTypeID(typeNumber) == CFNumberGetTypeID() &&
        CFNumberGetValue(typeNumber, kCFNumberIntType, &type) &&
        type == 4) {
        return true;
    }

    const void *layoutValue = CFDictionaryGetValue(spaceDict, CFSTR("TileLayoutManager"));
    if (layoutValue && CFGetTypeID(layoutValue) == CFDictionaryGetTypeID()) {
        return true;
    }

    const CFStringRef stringKeys[] = { CFSTR("type"), CFSTR("Type"), CFSTR("name"), CFSTR("Name") };
    for (size_t i = 0; i < sizeof(stringKeys) / sizeof(stringKeys[0]); i++) {
        const void *value = CFDictionaryGetValue(spaceDict, stringKeys[i]);
        if (value && CFGetTypeID(value) == CFStringGetTypeID()) {
            CFStringRef stringValue = (CFStringRef)value;
            if (CFStringFind(stringValue, CFSTR("Fullscreen"), kCFCompareCaseInsensitive).location != kCFNotFound ||
                CFStringFind(stringValue, CFSTR("Full Screen"), kCFCompareCaseInsensitive).location != kCFNotFound) {
                return true;
            }
        }
    }

    const CFStringRef boolKeys[] = { CFSTR("isFullscreen"), CFSTR("Is Fullscreen"), CFSTR("fullscreen") };
    for (size_t i = 0; i < sizeof(boolKeys) / sizeof(boolKeys[0]); i++) {
        const void *value = CFDictionaryGetValue(spaceDict, boolKeys[i]);
        if (value && CFGetTypeID(value) == CFBooleanGetTypeID() && CFBooleanGetValue((CFBooleanRef)value)) {
            return true;
        }
    }

    return false;
}

CFArrayRef iss_copy_cursor_display_app_spaces(void) {
    CGSSpaceID activeSpace = 0;
    bool hasActiveSpace = false;
    CFDictionaryRef displayDict = copy_target_display(true, &activeSpace, &hasActiveSpace);
    if (!displayDict) {
        return NULL;
    }

    const void *spacesValue = CFDictionaryGetValue(displayDict, CFSTR("Spaces"));
    if (!spacesValue || CFGetTypeID(spacesValue) != CFArrayGetTypeID()) {
        CFRelease(displayDict);
        return NULL;
    }

    CFStringRef displayID = (CFStringRef)CFDictionaryGetValue(displayDict, CFSTR("Display Identifier"));
    if (displayID && CFGetTypeID(displayID) != CFStringGetTypeID()) {
        displayID = NULL;
    }

    CFArrayRef windowList = CGWindowListCopyWindowInfo(kCGWindowListOptionOnScreenOnly, kCGNullWindowID);
    if (!windowList) {
        CFRelease(displayDict);
        return NULL;
    }

    CFArrayRef spaces = (CFArrayRef)spacesValue;
    CFIndex rawSpaceCount = CFArrayGetCount(spaces);
    unsigned int userVisibleCount = 0;
    for (CFIndex i = 0; i < rawSpaceCount; i++) {
        const void *spaceValue = CFArrayGetValueAtIndex(spaces, i);
        if (!spaceValue || CFGetTypeID(spaceValue) != CFDictionaryGetTypeID()) {
            continue;
        }
        CFDictionaryRef spaceDict = (CFDictionaryRef)spaceValue;
        CFNumberRef idNumber = (CFNumberRef)CFDictionaryGetValue(spaceDict, CFSTR("id64"));
        if (idNumber && CFGetTypeID(idNumber) == CFNumberGetTypeID()) {
            userVisibleCount++;
        }
    }

    CFMutableArrayRef result = CFArrayCreateMutable(NULL, 0, &kCFTypeArrayCallBacks);
    unsigned int index = 0;
    for (CFIndex i = 0; i < rawSpaceCount; i++) {
        const void *spaceValue = CFArrayGetValueAtIndex(spaces, i);
        if (!spaceValue || CFGetTypeID(spaceValue) != CFDictionaryGetTypeID()) {
            continue;
        }

        CFDictionaryRef spaceDict = (CFDictionaryRef)spaceValue;
        CFNumberRef idNumber = (CFNumberRef)CFDictionaryGetValue(spaceDict, CFSTR("id64"));
        if (!idNumber || CFGetTypeID(idNumber) != CFNumberGetTypeID()) {
            continue;
        }

        CGSSpaceID spaceID = 0;
        if (!CFNumberGetValue(idNumber, kCFNumberSInt64Type, &spaceID)) {
            index++;
            continue;
        }

        CFArrayRef pids = copy_pids_for_space(spaceDict);
        if (!pids) {
            index++;
            continue;
        }

        CFStringRef ownerName = NULL;
        bool isFullscreen = space_dict_is_fullscreen(spaceDict);

        CFIndex pidCount = CFArrayGetCount(pids);
        for (CFIndex pidIndex = 0; pidIndex < pidCount; pidIndex++) {
            CFNumberRef pidValue = (CFNumberRef)CFArrayGetValueAtIndex(pids, pidIndex);
            if (!pidValue || CFGetTypeID(pidValue) != CFNumberGetTypeID()) {
                continue;
            }

            int ownerPID = 0;
            if (!CFNumberGetValue(pidValue, kCFNumberIntType, &ownerPID) || ownerPID <= 0) {
                continue;
            }

            ownerName = copy_owner_name_for_pid(windowList, ownerPID);
            if (!ownerName) {
                continue;
            }

            CFMutableDictionaryRef item = CFDictionaryCreateMutable(NULL, 0, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
            CFNumberRef indexNumber = CFNumberCreate(NULL, kCFNumberIntType, &index);
            CFNumberRef countNumber = CFNumberCreate(NULL, kCFNumberIntType, &userVisibleCount);
            CFNumberRef spaceIDNumber = CFNumberCreate(NULL, kCFNumberSInt64Type, &spaceID);
            CFNumberRef ownerPIDNumber = CFNumberCreate(NULL, kCFNumberIntType, &ownerPID);

            CFDictionarySetValue(item, CFSTR("index"), indexNumber);
            CFDictionarySetValue(item, CFSTR("spaceCount"), countNumber);
            CFDictionarySetValue(item, CFSTR("spaceID"), spaceIDNumber);
            CFDictionarySetValue(item, CFSTR("ownerPID"), ownerPIDNumber);
            CFDictionarySetValue(item, CFSTR("ownerName"), ownerName);
            CFDictionarySetValue(item, CFSTR("isFullscreen"), isFullscreen ? kCFBooleanTrue : kCFBooleanFalse);
            if (displayID) {
                CFDictionarySetValue(item, CFSTR("displayID"), displayID);
            }

            CFArrayAppendValue(result, item);

            CFRelease(indexNumber);
            CFRelease(countNumber);
            CFRelease(spaceIDNumber);
            CFRelease(ownerPIDNumber);
            CFRelease(ownerName);
            CFRelease(item);
        }

        CFRelease(pids);
        index++;
    }

    CFRelease(windowList);
    CFRelease(displayDict);
    return result;
}

static bool iss_should_block_switch(const ISSSpaceInfo *info, ISSDirection direction) {
    if (!info) {
        return false;
    }
    if (info->spaceCount == 0) {
        return true;
    }

    unsigned int predicted;
    unsigned int current = get_prediction(info->displayID, &predicted) ? predicted : info->currentIndex;

    if (direction == ISSDirectionLeft) {
        return current == 0;
    }

    return current + 1 >= info->spaceCount;
}

bool iss_can_move(ISSSpaceInfo info, ISSDirection direction) {
    return !iss_should_block_switch(&info, direction);
}

static bool iss_post_dock_swipe(CGSGesturePhase phase, ISSDirection direction, double velocity) {
    const bool isRight = (direction == ISSDirectionRight);
    // Empirically, ±FLT_TRUE_MIN used in this way makes switching instant.
    const double progress = isRight ? (double)FLT_TRUE_MIN : -(double)FLT_TRUE_MIN;

    // Velocity of gesture based on speed setting
    const double vel = isRight ? velocity : -velocity;

    CGEventRef ev = CGEventCreate(NULL);
    if (!ev) {
        return false;
    }
    CGEventSetIntegerValueField(ev, kCGSEventTypeField, kCGSEventDockControl);
    CGEventSetIntegerValueField(ev, kCGEventGestureHIDType, kIOHIDEventTypeDockSwipe);
    CGEventSetIntegerValueField(ev, kCGEventGesturePhase, phase);
    CGEventSetDoubleValueField(ev, kCGEventGestureSwipeProgress, progress);
    CGEventSetIntegerValueField(ev, kCGEventGestureSwipeMotion, kCGGestureMotionHorizontal);
    CGEventSetDoubleValueField(ev, kCGEventGestureSwipeVelocityX, vel);
    CGEventSetDoubleValueField(ev, kCGEventGestureSwipeVelocityY, vel);
    CGEventPost(kCGSessionEventTap, ev);
    CFRelease(ev);
    return true;
}

static bool iss_perform_switch_gesture(ISSDirection direction, double velocity) {
    // Send three gesture events--began, changed, and ended
    // If we only send two then mission control doesn't work.
    return iss_post_dock_swipe(kCGSGesturePhaseBegan,   direction, velocity)
        && iss_post_dock_swipe(kCGSGesturePhaseChanged, direction, velocity)
        && iss_post_dock_swipe(kCGSGesturePhaseEnded,   direction, velocity);
}

/** @brief Walks a CGWindowListCopyWindowInfo result
 *
 * Used for trying to determine if Exposé or Mission Control is active.
 *
 * @param windowList The window list to scan
 * @param outLayer18Count The count of layer-18 windows
 * @param outLayer20Count The count of layer-20 windows
 */
static void scan_dock_window_list(CFArrayRef windowList,
                                  int *outLayer18Count,
                                  int *outLayer20Count) {
    *outLayer18Count = 0;
    *outLayer20Count = 0;
    CFIndex count = CFArrayGetCount(windowList);
    for (CFIndex i = 0; i < count; i++) {
        CFDictionaryRef info = (CFDictionaryRef)CFArrayGetValueAtIndex(windowList, i);
        CFStringRef owner = (CFStringRef)CFDictionaryGetValue(info, CFSTR("kCGWindowOwnerName"));
        if (!owner || !CFEqual(owner, CFSTR("Dock"))) continue;
        int layer = 0;
        CFNumberRef layerNum = (CFNumberRef)CFDictionaryGetValue(info, CFSTR("kCGWindowLayer"));
        if (layerNum) {
            CFNumberGetValue(layerNum, kCFNumberIntType, &layer);
        }
        if (layer == 18) {
            (*outLayer18Count)++;
            continue;
        }
        if (layer == 20) {
            (*outLayer20Count)++;
        }
    }
}

// Testable helpers
bool iss_is_expose_detected_in_window_list(CFArrayRef windowList) {
    int layer18Count = 0;
    int layer20Count = 0;
    scan_dock_window_list(windowList, &layer18Count, &layer20Count);
    // App Exposé: layer-18 present, at least one layer-20, AND count(layer=20) <= count(layer=18)
    return layer18Count > 0 && layer20Count > 0 && layer20Count <= layer18Count;
}

bool iss_is_mission_control_detected_in_window_list(CFArrayRef windowList) {
    int layer18Count = 0;
    int layer20Count = 0;
    scan_dock_window_list(windowList, &layer18Count, &layer20Count);
    // Mission Control: layer-18 present AND count(layer=20) > count(layer=18)
    return layer18Count > 0 && layer20Count > layer18Count;
}

/// Returns true when App Exposé is active (1-2 layer-20 windows)
/// This heuristic is empirical and may not work in all cases.
bool iss_is_expose_active(void) {
    if (!overlayDetectionEnabled) return false;
    CFArrayRef windowList = CGWindowListCopyWindowInfo(
        kCGWindowListOptionOnScreenOnly, kCGNullWindowID);
    if (!windowList) return false;
    bool result = iss_is_expose_detected_in_window_list(windowList);
    CFRelease(windowList);
    return result;
}

/// Returns true when Mission Control is active (3+ layer-20 windows)
/// This heuristic is empirical and may not work in all cases.
bool iss_is_mission_control_active(void) {
    if (!overlayDetectionEnabled) return false;
    CFArrayRef windowList = CGWindowListCopyWindowInfo(
        kCGWindowListOptionOnScreenOnly, kCGNullWindowID);
    if (!windowList) return false;
    bool result = iss_is_mission_control_detected_in_window_list(windowList);
    CFRelease(windowList);
    return result;
}

void iss_set_overlay_detection_enabled(bool enabled) {
    overlayDetectionEnabled = enabled;
}

bool iss_init(void) {
    if (globalTap) {
        return true;
    }

    if (!predictionsDict) {
        predictionsDict = CFDictionaryCreateMutable(NULL, 0, &kCFCopyStringDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
    }

    CGEventMask mask = CGEventMaskBit(kCGEventKeyDown) | CGEventMaskBit(kCGEventKeyUp)
        | (1ULL << kCGSEventGesture) | (1ULL << kCGSEventDockControl)
        | (1ULL << kCGSEventFluidTouchGesture);
    globalTap = CGEventTapCreate(
        kCGSessionEventTap,
        kCGHeadInsertEventTap,
        kCGEventTapOptionDefault,
        mask,
        eventTapCallback,
        NULL
    );

    if (!globalTap) {
        return false;
    }

    globalSource = CFMachPortCreateRunLoopSource(NULL, globalTap, 0);
    CFRunLoopAddSource(CFRunLoopGetMain(), globalSource, kCFRunLoopCommonModes);
    CGEventTapEnable(globalTap, true);

    return true;
}

void iss_destroy(void) {
    if (predictionsDict) {
        CFRelease(predictionsDict);
        predictionsDict = NULL;
    }
    if (globalTap) {
        CGEventTapEnable(globalTap, false);
        if (globalSource) {
            CFRunLoopRemoveSource(CFRunLoopGetMain(), globalSource, kCFRunLoopCommonModes);
            CFRelease(globalSource);
            globalSource = NULL;
        }
        CFRelease(globalTap);
        globalTap = NULL;
    }
}

bool iss_get_space_info(ISSSpaceInfo *info) {
    if (!info) {
        return false;
    }

    memset(info, 0, sizeof(*info));
    return load_space_info_for_display(info, true);
}

bool iss_get_menubar_space_info(ISSSpaceInfo *info) {
    if (!info) {
        return false;
    }

    memset(info, 0, sizeof(*info));
    return load_space_info_for_display(info, false);
}

static bool iss_switch_with_info(const ISSSpaceInfo *info, ISSDirection direction) {
    if (iss_should_block_switch(info, direction)) {
        return false;
    }
    if (!iss_perform_switch_gesture(direction, gestureSpeed)) {
        return false;
    }

    return true;
}

bool iss_switch(ISSDirection direction) {
    ISSSpaceInfo info;
    if (iss_get_space_info(&info)) {
        unsigned int predicted;
        unsigned int current = get_prediction(info.displayID, &predicted) ? predicted : info.currentIndex;
        unsigned int target = direction == ISSDirectionLeft ? current - 1 : current + 1;

        if (!iss_switch_with_info(&info, direction)) {
            return false;
        }
        set_prediction(info.displayID, target);
        if (switchCallback) { switchCallback(target); }
        return true;
    }

    return iss_perform_switch_gesture(direction, gestureSpeed);
}

bool iss_switch_to_index(unsigned int targetIndex) {
    ISSSpaceInfo info;
    if (!iss_get_space_info(&info)) {
        return false;
    }

    assert(info.spaceCount > 0);

    bool outOfBounds = targetIndex >= info.spaceCount;
    if (outOfBounds) {
        targetIndex = info.spaceCount - 1;
    }

    unsigned int predicted;
    unsigned int currentIndex = get_prediction(info.displayID, &predicted) ? predicted : info.currentIndex;

    if (currentIndex == targetIndex) {
        return !outOfBounds;
    }

    ISSDirection direction = currentIndex < targetIndex ? ISSDirectionRight : ISSDirectionLeft;
    unsigned int steps = direction == ISSDirectionRight ? (targetIndex - currentIndex) : (currentIndex - targetIndex);

    // Multiply velocity by number of steps for faster multi-space switching
    double velocity = gestureSpeed * steps;

    for (unsigned int i = 0; i < steps; i++) {
        if (!iss_perform_switch_gesture(direction, velocity)) {
            return false;
        }
    }

    set_prediction(info.displayID, targetIndex);
    if (switchCallback) { switchCallback(targetIndex); }
    return !outOfBounds;
}

void iss_set_swipe_override(bool enabled) {
    swipeOverrideEnabled = enabled;
    if (!enabled) {
        swipeTracking = false;
        swipeFired = false;
        suppressSwitchShortcutKeyUp = false;
    }
}

void iss_set_gesture_speed(double speed) {
    gestureSpeed = speed;
}

void iss_reset_predictions(void) {
    if (predictionsDict) {
        CFDictionaryRemoveAllValues(predictionsDict);
    }
}

void iss_set_switch_callback(ISSSwitchCallback callback) {
    switchCallback = callback;
}
