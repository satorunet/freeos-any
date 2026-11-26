/* Mac アプリ版: Cocoa の窓・入力・通信。OS 本体は host_efi.c が用意する UEFI の上で、別のスレッドで動く。 */
#import <Cocoa/Cocoa.h>
#include <pthread.h>
#include "host.h"

static pthread_mutex_t fbLock = PTHREAD_MUTEX_INITIALIZER;
static unsigned int shown[FB_W * FB_H];
static volatile int pendingDraw;
@class FBView;
static FBView *theView;

@interface FBView : NSView
@end
@implementation FBView {
    int buttons;
    double wheelAcc;
}
- (BOOL)acceptsFirstResponder { return YES; }
- (BOOL)isOpaque { return YES; }
- (NSRect)imageRect { /* 縦横の比を保って真ん中に */
    NSRect b = self.bounds; double s = MIN(b.size.width / FB_W, b.size.height / FB_H);
    double w = FB_W * s, h = FB_H * s;
    return NSMakeRect((b.size.width - w) / 2, (b.size.height - h) / 2, w, h);
}
- (void)drawRect:(NSRect)r {
    (void)r;
    [[NSColor blackColor] setFill]; NSRectFill(self.bounds);
    pthread_mutex_lock(&fbLock);
    CGColorSpaceRef cs = CGColorSpaceCreateDeviceRGB();
    CGContextRef bc = CGBitmapContextCreate(shown, FB_W, FB_H, 8, FB_W * 4, cs, kCGImageAlphaNoneSkipFirst | kCGBitmapByteOrder32Little);
    CGImageRef img = CGBitmapContextCreateImage(bc);
    pthread_mutex_unlock(&fbLock);
    CGContextRef c = [[NSGraphicsContext currentContext] CGContext];
    CGContextSetInterpolationQuality(c, kCGInterpolationMedium);
    CGContextDrawImage(c, NSRectToCGRect([self imageRect]), img);
    CGImageRelease(img); CGContextRelease(bc); CGColorSpaceRelease(cs);
    pendingDraw = 0;
}
- (void)sendMouse:(NSEvent *)e {
    NSPoint p = [self convertPoint:e.locationInWindow fromView:nil]; NSRect ir = [self imageRect];
    int x = (int)((p.x - ir.origin.x) * FB_W / ir.size.width), y = (int)((ir.origin.y + ir.size.height - p.y) * FB_H / ir.size.height);
    x = x < 0 ? 0 : x >= FB_W ? FB_W - 1 : x; y = y < 0 ? 0 : y >= FB_H ? FB_H - 1 : y;
    host_mouse(x, y, buttons);
}
- (void)mouseMoved:(NSEvent *)e { [self sendMouse:e]; }
- (void)mouseDragged:(NSEvent *)e { [self sendMouse:e]; }
- (void)mouseDown:(NSEvent *)e { buttons |= 1; [self sendMouse:e]; }
- (void)mouseUp:(NSEvent *)e { buttons &= ~1; [self sendMouse:e]; }
- (void)rightMouseDown:(NSEvent *)e { buttons |= 2; [self sendMouse:e]; }
- (void)rightMouseUp:(NSEvent *)e { buttons &= ~2; [self sendMouse:e]; }
- (void)scrollWheel:(NSEvent *)e {
    wheelAcc += e.hasPreciseScrollingDeltas ? e.scrollingDeltaY / 20.0 : e.scrollingDeltaY;
    int n = (int)wheelAcc; if (n) { host_wheel(n); wheelAcc -= n; }
}
- (void)keyDown:(NSEvent *)e {
    static const struct { unsigned short code; int scan, ch; } map[] = {
        {53, 0x17, 0}, {36, 0, 13}, {76, 0, 13}, {48, 0, 9}, {51, 0, 8}, {117, 8, 0},
        {123, 4, 0}, {124, 3, 0}, {125, 2, 0}, {126, 1, 0}, {115, 5, 0}, {119, 6, 0}, {116, 9, 0}, {121, 10, 0}, {114, 7, 0},
        {122, 0x0B, 0}, {120, 0x0C, 0}, {99, 0x0D, 0}, {118, 0x0E, 0}, {96, 0x0F, 0}, {97, 0x10, 0}, {98, 0x11, 0}, {100, 0x12, 0},
        {101, 0x13, 0}, {109, 0x14, 0}, {103, 0x15, 0}, {111, 0x16, 0},
        {104, 0x100, 0}, {102, 0x101, 0}, /* 「かな」「英数」 */
    };
    for (unsigned i = 0; i < sizeof map / sizeof *map; i++) if (map[i].code == e.keyCode) { host_key(map[i].scan, map[i].ch); return; }
    if (e.modifierFlags & NSEventModifierFlagCommand) { [super keyDown:e]; return; }
    NSString *s = e.characters;
    for (NSUInteger i = 0; i < s.length; i++) { unichar c = [s characterAtIndex:i]; if (c >= 32 && c < 0xF700) host_key(0, c); else if (c < 32) host_key(0, c); }
}
- (void)flagsChanged:(NSEvent *)e { (void)e; }
@end

void host_present(const unsigned int *pixels, int w, int h) {
    pthread_mutex_lock(&fbLock); memcpy(shown, pixels, (size_t)w * h * 4); pthread_mutex_unlock(&fbLock);
    if (!pendingDraw) { pendingDraw = 1; dispatch_async(dispatch_get_main_queue(), ^{ [theView setNeedsDisplay:YES]; }); }
}
void host_quit(int reboot) { (void)reboot; dispatch_async(dispatch_get_main_queue(), ^{ [NSApp terminate:nil]; }); }
unsigned long long host_memory_bytes(void) { return [NSProcessInfo processInfo].physicalMemory; }

/* ---- 通信 ---- */
@interface ReqRouter : NSObject <NSURLSessionDataDelegate> + (instancetype)shared; @end
@interface Req : NSObject <NSURLSessionDataDelegate, NSURLSessionTaskDelegate>
@property NSMutableData *data; @property NSHTTPURLResponse *resp; @property NSError *err; @property NSMutableString *setCookie;
@property volatile int done; @property NSURLSessionDataTask *task;
@end
@implementation Req
- (void)collectCookies:(NSHTTPURLResponse *)r {
    NSArray *cs = [NSHTTPCookie cookiesWithResponseHeaderFields:r.allHeaderFields forURL:r.URL];
    for (NSHTTPCookie *c in cs) { /* OS のクッキー係に渡す形 */
        [self.setCookie appendFormat:@"%@=%@; Path=%@", c.name, c.value, c.path ?: @"/"];
        if (![c.domain hasPrefix:r.URL.host ?: @""] || [c.domain hasPrefix:@"."]) [self.setCookie appendFormat:@"; Domain=%@", c.domain];
        if (c.expiresDate) [self.setCookie appendFormat:@"; Max-Age=%ld", (long)MAX(0, [c.expiresDate timeIntervalSinceNow])];
        if (c.isSecure) [self.setCookie appendString:@"; Secure"];
        if (c.isHTTPOnly) [self.setCookie appendString:@"; HttpOnly"];
        [self.setCookie appendString:@"\n"];
    }
}
- (void)URLSession:(NSURLSession *)s task:(NSURLSessionTask *)t willPerformHTTPRedirection:(NSHTTPURLResponse *)r newRequest:(NSURLRequest *)req completionHandler:(void (^)(NSURLRequest *))done {
    (void)s; (void)t; [self collectCookies:r]; done(req);
}
- (void)URLSession:(NSURLSession *)s dataTask:(NSURLSessionDataTask *)t didReceiveResponse:(NSURLResponse *)r completionHandler:(void (^)(NSURLSessionResponseDisposition))done {
    (void)s; (void)t; self.resp = (NSHTTPURLResponse *)r; [self collectCookies:self.resp]; done(NSURLSessionResponseAllow);
}
- (void)URLSession:(NSURLSession *)s dataTask:(NSURLSessionDataTask *)t didReceiveData:(NSData *)d { (void)s; (void)t; [self.data appendData:d]; }
- (void)URLSession:(NSURLSession *)s task:(NSURLSessionTask *)t didCompleteWithError:(NSError *)e { (void)s; (void)t; self.err = e; self.done = 1; }
@end
static NSMutableDictionary<NSNumber *, Req *> *reqs;
static NSURLSession *session;
static int nextId;
int host_http_start(const char *url, const char *cookie) {
    @autoreleasepool {
        NSURL *u = [NSURL URLWithString:[NSString stringWithUTF8String:url]];
        if (!u) u = [NSURL URLWithString:[[NSString stringWithUTF8String:url] stringByAddingPercentEncodingWithAllowedCharacters:NSCharacterSet.URLQueryAllowedCharacterSet]];
        if (!u) return -1;
        static dispatch_once_t once; static Req *dummy;
        dispatch_once(&once, ^{
            reqs = [NSMutableDictionary dictionary];
            NSURLSessionConfiguration *c = [NSURLSessionConfiguration ephemeralSessionConfiguration];
            c.HTTPShouldSetCookies = NO; c.HTTPCookieAcceptPolicy = NSHTTPCookieAcceptPolicyNever; c.timeoutIntervalForRequest = 30;
            dummy = [Req new];
            session = [NSURLSession sessionWithConfiguration:c delegate:(id)[ReqRouter shared] delegateQueue:nil];
        });
        (void)dummy;
        NSMutableURLRequest *r = [NSMutableURLRequest requestWithURL:u];
        [r setValue:@"Mozilla/5.0 (compatible; FreeOSAny; Mac)" forHTTPHeaderField:@"User-Agent"];
        if (cookie && *cookie) [r setValue:[NSString stringWithUTF8String:cookie] forHTTPHeaderField:@"Cookie"];
        Req *q = [Req new]; q.data = [NSMutableData data]; q.setCookie = [NSMutableString string];
        int id_;
        @synchronized (reqs) { id_ = ++nextId; reqs[@(id_)] = q; }
        q.task = [session dataTaskWithRequest:r];
        q.task.taskDescription = [NSString stringWithFormat:@"%d", id_];
        [q.task resume];
        return id_;
    }
}
int host_http_poll(int id, int *status, char *type, int typeCap, char *finalUrl, int urlCap, char *setCookie, int scCap,
                   unsigned char *buf, unsigned long long cap, unsigned long long *len, int *truncated, char *err, int errCap) {
    @autoreleasepool {
        Req *q; @synchronized (reqs) { q = reqs[@(id)]; }
        if (!q) return -1;
        if (!q.done) return 0;
        @synchronized (reqs) { [reqs removeObjectForKey:@(id)]; }
        if (q.err) { snprintf(err, errCap, "%s", q.err.localizedDescription.UTF8String ?: "通信できません"); return -1; }
        *status = (int)q.resp.statusCode;
        snprintf(type, typeCap, "%s", [q.resp valueForHTTPHeaderField:@"Content-Type"].UTF8String ?: "");
        snprintf(finalUrl, urlCap, "%s", q.resp.URL.absoluteString.UTF8String ?: "");
        snprintf(setCookie, scCap, "%s", q.setCookie.UTF8String ?: "");
        unsigned long long n = q.data.length; *truncated = n > cap; if (n > cap) n = cap;
        memcpy(buf, q.data.bytes, (size_t)n); *len = n;
        return 1;
    }
}
void host_http_cancel(int id) { @synchronized (reqs) { Req *q = reqs[@(id)]; [q.task cancel]; [reqs removeObjectForKey:@(id)]; } }

/* セッションの通知を、それぞれの Req に振り分ける */
@implementation ReqRouter
+ (instancetype)shared { static ReqRouter *r; static dispatch_once_t o; dispatch_once(&o, ^{ r = [ReqRouter new]; }); return r; }
- (Req *)reqFor:(NSURLSessionTask *)t { Req *q; @synchronized (reqs) { q = reqs[@(t.taskDescription.intValue)]; } return q; }
- (void)URLSession:(NSURLSession *)s task:(NSURLSessionTask *)t willPerformHTTPRedirection:(NSHTTPURLResponse *)r newRequest:(NSURLRequest *)req completionHandler:(void (^)(NSURLRequest *))done {
    Req *q = [self reqFor:t]; if (q) [q URLSession:s task:t willPerformHTTPRedirection:r newRequest:req completionHandler:done]; else done(req);
}
- (void)URLSession:(NSURLSession *)s dataTask:(NSURLSessionDataTask *)t didReceiveResponse:(NSURLResponse *)r completionHandler:(void (^)(NSURLSessionResponseDisposition))done {
    Req *q = [self reqFor:t]; if (q) [q URLSession:s dataTask:t didReceiveResponse:r completionHandler:done]; else done(NSURLSessionResponseCancel);
}
- (void)URLSession:(NSURLSession *)s dataTask:(NSURLSessionDataTask *)t didReceiveData:(NSData *)d { Req *q = [self reqFor:t]; [q URLSession:s dataTask:t didReceiveData:d]; }
- (void)URLSession:(NSURLSession *)s task:(NSURLSessionTask *)t didCompleteWithError:(NSError *)e { Req *q = [self reqFor:t]; [q URLSession:s task:t didCompleteWithError:e]; }
@end

/* ---- アプリ ---- */
@interface App : NSObject <NSApplicationDelegate, NSWindowDelegate> @property NSWindow *win; @end
@implementation App
- (void)applicationDidFinishLaunching:(NSNotification *)n {
    (void)n;
    NSRect r = NSMakeRect(0, 0, FB_W, FB_H);
    NSScreen *scr = NSScreen.mainScreen; double s = 1;
    if (scr && (scr.visibleFrame.size.width < FB_W + 40 || scr.visibleFrame.size.height < FB_H + 60)) s = MIN((scr.visibleFrame.size.width - 40) / FB_W, (scr.visibleFrame.size.height - 60) / FB_H);
    r.size.width *= s; r.size.height *= s;
    self.win = [[NSWindow alloc] initWithContentRect:r styleMask:NSWindowStyleMaskTitled | NSWindowStyleMaskClosable | NSWindowStyleMaskMiniaturizable | NSWindowStyleMaskResizable backing:NSBackingStoreBuffered defer:NO];
    self.win.title = @"FreeOS Any"; self.win.delegate = self; self.win.acceptsMouseMovedEvents = YES;
    self.win.contentAspectRatio = NSMakeSize(FB_W, FB_H);
    theView = [[FBView alloc] initWithFrame:r];
    self.win.contentView = theView; [self.win makeFirstResponder:theView];
    [self.win center]; [self.win makeKeyAndOrderFront:nil];
    [NSApp activateIgnoringOtherApps:YES];
    /* データのフォルダ。アプリの中にモデルがあれば、そこを指すリンクを置く (コピーしない) */
    NSString *dir = [NSHomeDirectory() stringByAppendingPathComponent:@"Library/Application Support/FreeOS Any/disk"];
    NSString *ai = [dir stringByAppendingPathComponent:@"HOME/AI"];
    [[NSFileManager defaultManager] createDirectoryAtPath:ai withIntermediateDirectories:YES attributes:nil error:nil];
    NSMutableArray *srcs = [NSMutableArray array];
    if (NSBundle.mainBundle.resourcePath) [srcs addObject:[NSBundle.mainBundle.resourcePath stringByAppendingPathComponent:@"AI"]];
    if (getenv("FREEOS_MODEL_DIR")) [srcs addObject:@(getenv("FREEOS_MODEL_DIR"))];
    for (NSString *src in srcs) for (NSString *f in [[NSFileManager defaultManager] contentsOfDirectoryAtPath:src error:nil]) {
        if (![f hasSuffix:@".fqw"]) continue;
        NSString *to = [ai stringByAppendingPathComponent:f];
        if (![[NSFileManager defaultManager] fileExistsAtPath:to]) [[NSFileManager defaultManager] createSymbolicLinkAtPath:to withDestinationPath:[src stringByAppendingPathComponent:f] error:nil];
    }
    NSString *samples = [NSBundle.mainBundle.resourcePath stringByAppendingPathComponent:@"samples"];
    for (NSString *f in [[NSFileManager defaultManager] contentsOfDirectoryAtPath:samples error:nil]) {
        NSString *to = [[dir stringByAppendingPathComponent:@"HOME"] stringByAppendingPathComponent:f];
        if (![[NSFileManager defaultManager] fileExistsAtPath:to]) [[NSFileManager defaultManager] copyItemAtPath:[samples stringByAppendingPathComponent:f] toPath:to error:nil];
    }
    host_start(dir.fileSystemRepresentation);
    if (getenv("FREEOS_SNAPSHOT")) { /* 試験用: 2 秒ごとに画面を PPM で書き出す */
        NSString *path = @(getenv("FREEOS_SNAPSHOT"));
        [NSTimer scheduledTimerWithTimeInterval:2 repeats:YES block:^(NSTimer *t) {
            (void)t; FILE *f = fopen(path.fileSystemRepresentation, "wb"); if (!f) return;
            fprintf(f, "P6 %d %d 255\n", FB_W, FB_H);
            pthread_mutex_lock(&fbLock);
            for (int i = 0; i < FB_W * FB_H; i++) { unsigned v = shown[i]; unsigned char rgb[3] = {(unsigned char)(v >> 16), (unsigned char)(v >> 8), (unsigned char)v}; fwrite(rgb, 1, 3, f); }
            pthread_mutex_unlock(&fbLock); fclose(f);
        }];
    }
}
- (BOOL)applicationShouldTerminateAfterLastWindowClosed:(NSApplication *)a { (void)a; return YES; }
@end

int main(int argc, const char **argv) {
    (void)argc; (void)argv;
    @autoreleasepool {
        NSApplication *app = [NSApplication sharedApplication];
        app.activationPolicy = NSApplicationActivationPolicyRegular;
        NSMenu *bar = [NSMenu new]; NSMenuItem *item = [NSMenuItem new]; [bar addItem:item];
        NSMenu *m = [NSMenu new]; [m addItemWithTitle:@"FreeOS Any を終了" action:@selector(terminate:) keyEquivalent:@"q"]; item.submenu = m;
        app.mainMenu = bar;
        App *d = [App new]; app.delegate = d;
        [app run];
    }
    return 0;
}
