#include "AsyncFileDialogMac.hpp"

#import <AppKit/AppKit.h>
#import <UniformTypeIdentifiers/UniformTypeIdentifiers.h>

#include <algorithm>

namespace quadmapper {

namespace {

// The panels on screen, by the dialog that asked for each: several AsyncFileDialog
// objects can have one open at once. Touched on the main thread only.
NSMutableDictionary<NSValue*, NSSavePanel*>* openPanels() {
    static NSMutableDictionary<NSValue*, NSSavePanel*>* panels = [NSMutableDictionary dictionary];
    return panels;
}

NSString* toNSString(const std::string& utf8) {
    return [[NSString alloc] initWithBytes:utf8.data() length:utf8.size() encoding:NSUTF8StringEncoding];
}

// The extensions in a Win32 filter ("Name\0*.a;*.b\0Name\0*.c\0"), or nil when the
// filter is empty or has an "all files" entry: then the panel offers every file.
NSArray<UTType*>* typesFromFilter(const std::string& filter) {
    NSMutableArray<UTType*>* types = [NSMutableArray array];
    size_t start = 0;
    for (int part = 0; start < filter.size(); ++part) {
        const size_t end = std::min(filter.find('\0', start), filter.size());
        const std::string text = filter.substr(start, end - start);
        start = end + 1;
        if (part % 2 == 0) continue;  // the even parts are the display names
        size_t at = 0;
        while (at < text.size()) {
            const size_t stop = std::min(text.find(';', at), text.size());
            const std::string pattern = text.substr(at, stop - at);
            at = stop + 1;
            const size_t dot = pattern.rfind('.');
            const std::string extension = dot == std::string::npos ? std::string() : pattern.substr(dot + 1);
            if (extension.empty() || extension.find('*') != std::string::npos) return nil;
            if (UTType* type = [UTType typeWithFilenameExtension:toNSString(extension)]) {
                [types addObject:type];
            }
        }
    }
    return types.count > 0 ? types : nil;
}

NSSavePanel* makePanel(const AsyncFileDialog::Request& request) {
    using Kind = AsyncFileDialog::Kind;
    NSSavePanel* panel = nil;
    if (request.kind == Kind::SaveFile) {
        panel = [NSSavePanel savePanel];  // asks before replacing a file by itself
        if (!request.defaultExt.empty()) {
            if (UTType* type = [UTType typeWithFilenameExtension:toNSString(request.defaultExt)]) {
                panel.allowedContentTypes = @[type];  // also adds the extension when none is typed
            }
        }
    } else {
        NSOpenPanel* open = [NSOpenPanel openPanel];
        const bool folder = request.kind == Kind::PickFolder;
        open.canChooseFiles = !folder;
        open.canChooseDirectories = folder;
        open.allowsMultipleSelection = request.kind == Kind::OpenFiles;
        if (!folder) {
            if (NSArray<UTType*>* types = typesFromFilter(request.filter)) open.allowedContentTypes = types;
        }
        panel = open;
    }
    panel.canCreateDirectories = YES;
    NSString* title = toNSString(request.title);
    panel.title = title;
    if (request.kind != Kind::SaveFile) panel.message = title;  // an open panel shows no title bar

    // initialPath is a folder to start in, or a file whose folder and name are offered.
    if (!request.initialPath.empty()) {
        NSString* path = toNSString(request.initialPath);
        BOOL isFolder = NO;
        const BOOL exists = [NSFileManager.defaultManager fileExistsAtPath:path isDirectory:&isFolder];
        if (exists && isFolder) {
            panel.directoryURL = [NSURL fileURLWithPath:path isDirectory:YES];
        } else {
            NSString* parent = path.stringByDeletingLastPathComponent;
            if (parent.length > 0) panel.directoryURL = [NSURL fileURLWithPath:parent isDirectory:YES];
            if (request.kind == Kind::SaveFile) panel.nameFieldStringValue = path.lastPathComponent;
        }
    }
    return panel;
}

}  // namespace

std::vector<std::string> macRunFileDialog(const AsyncFileDialog::Request& request, const void* owner) {
    __block std::vector<std::string> chosen;
    dispatch_semaphore_t answered = dispatch_semaphore_create(0);
    const AsyncFileDialog::Request* asked = &request;  // valid until the wait below returns

    dispatch_async(dispatch_get_main_queue(), ^{
        NSSavePanel* panel = makePanel(*asked);
        const bool isOpenPanel = asked->kind != AsyncFileDialog::Kind::SaveFile;
        NSValue* key = [NSValue valueWithPointer:owner];
        openPanels()[key] = panel;
        [panel beginWithCompletionHandler:^(NSModalResponse response) {
            if (response == NSModalResponseOK) {
                NSArray<NSURL*>* urls = isOpenPanel ? ((NSOpenPanel*)panel).URLs
                                                    : (panel.URL ? @[panel.URL] : @[]);
                for (NSURL* url in urls) {
                    if (const char* path = url.fileSystemRepresentation) chosen.emplace_back(path);
                }
            }
            [openPanels() removeObjectForKey:key];
            dispatch_semaphore_signal(answered);
        }];
    });

    dispatch_semaphore_wait(answered, DISPATCH_TIME_FOREVER);
    return chosen;
}

void macCancelFileDialog(const void* owner) {
    if (NSSavePanel* panel = openPanels()[[NSValue valueWithPointer:owner]]) [panel cancel:nil];
    // Runs a panel request that is still queued, and delivers the cancelled answer.
    CFRunLoopRunInMode(kCFRunLoopDefaultMode, 0.02, true);
}

}  // namespace quadmapper
