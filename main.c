#define _GNU_SOURCE

#include <X11/Xlib.h>
#include <X11/Xatom.h>
#include <X11/cursorfont.h>
#include <X11/extensions/Xrandr.h>
#include <X11/extensions/XShm.h>
#include <X11/keysym.h>

#include <pulse/error.h>
#include <pulse/simple.h>

#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/channel_layout.h>
#include <libavutil/dict.h>
#include <libavutil/error.h>
#include <libavutil/frame.h>
#include <libavutil/hwcontext.h>
#include <libavutil/hwcontext_vaapi.h>
#include <libavutil/imgutils.h>
#include <libavutil/mathematics.h>
#include <libavutil/opt.h>
#include <libavutil/samplefmt.h>

#include <errno.h>
#include <limits.h>
#include <pthread.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ipc.h>
#include <sys/shm.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#define DefaultFrameRate 60
#define DefaultVideoBitrate 12000000
#define DefaultAudioBitrate 160000
#define AudioSampleRate 48000
#define AudioChannels 2
#define AudioFrameSamples 1024

#define CheckResult(Result, Operation) \
    do { \
        if ((Result) < 0) { \
            printError((Operation), (Result)); \
            return false; \
        } \
    } while (0)

enum CaptureMode {
    CaptureScreen,
    CaptureWindow,
    CaptureArea,
    CaptureMonitor
};

typedef struct RecorderState RecorderState;

typedef struct {
    Display *DisplayHandle;
    Window Drawable;
    XShmSegmentInfo ShmInfo;
    XImage *Image;
    int SourceX;
    int SourceY;
    unsigned Width;
    unsigned Height;
    enum AVPixelFormat PixelFormat;
    bool Attached;
} X11Capture;

typedef struct {
    RecorderState *State;
    AVCodecContext *Encoder;
    AVStream *Stream;
    AVBufferRef *HwDevice;
    AVBufferRef *HwFrames;
    enum AVPixelFormat SourcePixelFormat;
    enum AVPixelFormat SoftwarePixelFormat;
    bool UseVaapi;
    int64_t NextPts;
    pthread_t Thread;
    bool Started;
} VideoWorker;

typedef struct {
    RecorderState *State;
    pa_simple *Pulse;
    AVCodecContext *Encoder;
    AVStream *Stream;
    int64_t NextPts;
    const char *DeviceName;
    const char *Description;
    pthread_t Thread;
    bool Started;
} AudioWorker;

struct RecorderState {
    Display *DisplayHandle;
    Window RootWindow;
    Window SelectedWindow;
    enum CaptureMode CaptureMode;
    unsigned MonitorNumber;
    int CaptureX;
    int CaptureY;
    unsigned CaptureWidth;
    unsigned CaptureHeight;

    X11Capture Capture;
    VideoWorker Video;
    AudioWorker DesktopAudio;
    AudioWorker Microphone;

    AVFormatContext *Output;
    pthread_mutex_t MuxMutex;
    atomic_bool StopRequested;
    atomic_bool FatalError;

    char OutputDirectory[PATH_MAX];
    char OutputPath[PATH_MAX];
    char CodecName[128];
    bool CodecExplicit;
    int FrameRate;
    int VideoBitrate;
    int AudioBitrate;
};

static RecorderState *GlobalState;

static void printError(const char *Operation, int ErrorCode)
{
    char Buffer[AV_ERROR_MAX_STRING_SIZE];
    av_strerror(ErrorCode, Buffer, sizeof(Buffer));
    fprintf(stderr, "%s: %s\n", Operation, Buffer);
}

static void requestStop(int SignalNumber)
{
    (void)SignalNumber;
    if (GlobalState != NULL) {
        atomic_store_explicit(&GlobalState->StopRequested, true, memory_order_relaxed);
    }
}

static void requestFatal(RecorderState *State)
{
    atomic_store_explicit(&State->FatalError, true, memory_order_relaxed);
    atomic_store_explicit(&State->StopRequested, true, memory_order_relaxed);
}

static Window findTopLevelWindow(Display *DisplayHandle, Window WindowId)
{
    Window Root = None;
    Window Parent = None;
    Window *Children = NULL;
    unsigned ChildCount = 0;

    while (WindowId != None) {
        if (!XQueryTree(
            DisplayHandle,
            WindowId,
            &Root,
            &Parent,
            &Children,
            &ChildCount
        )) {
            break;
        }

        if (Children != NULL) {
            XFree(Children);
        }

        if (Parent == None || Parent == Root) {
            break;
        }

        WindowId = Parent;
    }

    return WindowId;
}

static bool selectWindow(RecorderState *State)
{
    Display *DisplayHandle = State->DisplayHandle;
    Cursor SelectionCursor = XCreateFontCursor(DisplayHandle, XC_crosshair);

    if (SelectionCursor == None) {
        fprintf(stderr, "Could not create the selection cursor.\n");
        return false;
    }

    int GrabResult = XGrabPointer(
        DisplayHandle,
        State->RootWindow,
        False,
        ButtonPressMask,
        GrabModeAsync,
        GrabModeAsync,
        None,
        SelectionCursor,
        CurrentTime
    );

    if (GrabResult != GrabSuccess) {
        fprintf(stderr, "Could not grab the X11 pointer.\n");
        XFreeCursor(DisplayHandle, SelectionCursor);
        return false;
    }

    int KeyboardResult = XGrabKeyboard(
        DisplayHandle,
        State->RootWindow,
        False,
        GrabModeAsync,
        GrabModeAsync,
        CurrentTime
    );

    printf("Click the window to record. Press Escape to cancel.\n");
    fflush(stdout);
    XFlush(DisplayHandle);

    bool Selected = false;
    XEvent Event;

    while (!Selected) {
        XNextEvent(DisplayHandle, &Event);

        if (Event.type == KeyPress) {
            if (XLookupKeysym(&Event.xkey, 0) == XK_Escape) {
                break;
            }
            continue;
        }

        if (Event.type != ButtonPress || Event.xbutton.button != Button1) {
            continue;
        }

        Window WindowId = Event.xbutton.subwindow;
        if (WindowId == None) {
            continue;
        }

        WindowId = findTopLevelWindow(DisplayHandle, WindowId);
        XWindowAttributes Attributes;

        if (!XGetWindowAttributes(DisplayHandle, WindowId, &Attributes)) {
            fprintf(stderr, "Could not query the selected window.\n");
            break;
        }

        if (Attributes.width < 2 || Attributes.height < 2) {
            fprintf(stderr, "The selected window is too small.\n");
            break;
        }

        State->SelectedWindow = WindowId;
        State->CaptureWidth = (unsigned)Attributes.width & ~1U;
        State->CaptureHeight = (unsigned)Attributes.height & ~1U;
        Selected = true;
    }

    if (KeyboardResult == GrabSuccess) {
        XUngrabKeyboard(DisplayHandle, CurrentTime);
    }
    XUngrabPointer(DisplayHandle, CurrentTime);
    XFreeCursor(DisplayHandle, SelectionCursor);
    XFlush(DisplayHandle);

    return Selected;
}

static void drawSelectionRectangle(
    Display *DisplayHandle,
    GC GraphicsContext,
    int X1,
    int Y1,
    int X2,
    int Y2
)
{
    int Left = X1 < X2 ? X1 : X2;
    int Top = Y1 < Y2 ? Y1 : Y2;
    int Right = X1 > X2 ? X1 : X2;
    int Bottom = Y1 > Y2 ? Y1 : Y2;

    unsigned Width = (unsigned)(Right - Left);
    unsigned Height = (unsigned)(Bottom - Top);

    if (Width == 0 || Height == 0) {
        return;
    }

    Window Root = DefaultRootWindow(DisplayHandle);
    XDrawRectangle(DisplayHandle, Root, GraphicsContext, Left, Top, Width, Height);
    XFlush(DisplayHandle);
}

static bool selectArea(RecorderState *State)
{
    Display *DisplayHandle = State->DisplayHandle;
    Cursor SelectionCursor = XCreateFontCursor(DisplayHandle, XC_crosshair);

    if (SelectionCursor == None) {
        return false;
    }

    GC GraphicsContext = XCreateGC(DisplayHandle, State->RootWindow, 0, NULL);
    if (GraphicsContext == NULL) {
        XFreeCursor(DisplayHandle, SelectionCursor);
        return false;
    }

    int Screen = DefaultScreen(DisplayHandle);
    XSetFunction(DisplayHandle, GraphicsContext, GXxor);
    XSetForeground(
        DisplayHandle,
        GraphicsContext,
        WhitePixel(DisplayHandle, Screen) ^ BlackPixel(DisplayHandle, Screen)
    );

    int GrabResult = XGrabPointer(
        DisplayHandle,
        State->RootWindow,
        False,
        ButtonPressMask | ButtonReleaseMask | PointerMotionMask,
        GrabModeAsync,
        GrabModeAsync,
        None,
        SelectionCursor,
        CurrentTime
    );

    if (GrabResult != GrabSuccess) {
        XFreeGC(DisplayHandle, GraphicsContext);
        XFreeCursor(DisplayHandle, SelectionCursor);
        return false;
    }

    int KeyboardResult = XGrabKeyboard(
        DisplayHandle,
        State->RootWindow,
        False,
        GrabModeAsync,
        GrabModeAsync,
        CurrentTime
    );

    printf("Drag the area to record. Press Escape to cancel.\n");
    fflush(stdout);
    XFlush(DisplayHandle);

    int StartX = 0;
    int StartY = 0;
    int CurrentX = 0;
    int CurrentY = 0;
    bool Dragging = false;
    bool Drawn = false;
    bool Selected = false;

    XEvent Event;

    while (!Selected) {
        XNextEvent(DisplayHandle, &Event);

        if (Event.type == KeyPress) {
            if (XLookupKeysym(&Event.xkey, 0) == XK_Escape) {
                break;
            }
            continue;
        }

        if (Event.type == ButtonPress && Event.xbutton.button == Button1 && !Dragging) {
            StartX = CurrentX = Event.xbutton.x_root;
            StartY = CurrentY = Event.xbutton.y_root;
            Dragging = true;
            Drawn = false;
            continue;
        }

        if (Event.type == MotionNotify && Dragging) {
            if (Drawn) {
                drawSelectionRectangle(DisplayHandle, GraphicsContext, StartX, StartY, CurrentX, CurrentY);
            }

            CurrentX = Event.xmotion.x_root;
            CurrentY = Event.xmotion.y_root;
            drawSelectionRectangle(DisplayHandle, GraphicsContext, StartX, StartY, CurrentX, CurrentY);
            Drawn = true;
            continue;
        }

        if (Event.type == ButtonRelease && Event.xbutton.button == Button1 && Dragging) {
            if (Drawn) {
                drawSelectionRectangle(DisplayHandle, GraphicsContext, StartX, StartY, CurrentX, CurrentY);
            }

            CurrentX = Event.xbutton.x_root;
            CurrentY = Event.xbutton.y_root;

            int Left = StartX < CurrentX ? StartX : CurrentX;
            int Top = StartY < CurrentY ? StartY : CurrentY;
            int Right = StartX > CurrentX ? StartX : CurrentX;
            int Bottom = StartY > CurrentY ? StartY : CurrentY;

            unsigned Width = (unsigned)(Right - Left) & ~1U;
            unsigned Height = (unsigned)(Bottom - Top) & ~1U;

            Dragging = false;
            Drawn = false;

            if (Width < 2 || Height < 2) {
                fprintf(stderr, "Selected area is too small.\n");
                continue;
            }

            State->CaptureX = Left;
            State->CaptureY = Top;
            State->CaptureWidth = Width;
            State->CaptureHeight = Height;
            Selected = true;
        }
    }

    if (KeyboardResult == GrabSuccess) {
        XUngrabKeyboard(DisplayHandle, CurrentTime);
    }
    XUngrabPointer(DisplayHandle, CurrentTime);
    XFreeGC(DisplayHandle, GraphicsContext);
    XFreeCursor(DisplayHandle, SelectionCursor);
    XFlush(DisplayHandle);

    return Selected;
}

static bool getMonitorGeometry(RecorderState *State, unsigned MonitorNumber)
{
    int MonitorCount = 0;
    XRRMonitorInfo *Monitors = XRRGetMonitors(
        State->DisplayHandle,
        State->RootWindow,
        True,
        &MonitorCount
    );

    if (Monitors == NULL || MonitorCount <= 0) {
        fprintf(stderr, "No active X11 monitors were found.\n");
        if (Monitors != NULL) {
            XRRFreeMonitors(Monitors);
        }
        return false;
    }

    if (MonitorNumber == 0 || MonitorNumber > (unsigned)MonitorCount) {
        fprintf(stderr, "Monitor %u does not exist. Available monitors: %d.\n", MonitorNumber, MonitorCount);
        XRRFreeMonitors(Monitors);
        return false;
    }

    XRRMonitorInfo *Monitor = &Monitors[MonitorNumber - 1];
    State->CaptureX = Monitor->x;
    State->CaptureY = Monitor->y;
    State->CaptureWidth = (unsigned)Monitor->width & ~1U;
    State->CaptureHeight = (unsigned)Monitor->height & ~1U;

    printf("Selected monitor %u: %ux%u+%d+%d\n",
        MonitorNumber,
        State->CaptureWidth,
        State->CaptureHeight,
        State->CaptureX,
        State->CaptureY);

    if (Monitor->name != None) {
        char *Name = XGetAtomName(State->DisplayHandle, Monitor->name);
        if (Name != NULL) {
            printf("Monitor name: %s\n", Name);
            XFree(Name);
        }
    }

    XRRFreeMonitors(Monitors);
    return State->CaptureWidth >= 2 && State->CaptureHeight >= 2;
}

static void listMonitors(Display *DisplayHandle)
{
    Window Root = RootWindow(DisplayHandle, DefaultScreen(DisplayHandle));
    int MonitorCount = 0;
    XRRMonitorInfo *Monitors = XRRGetMonitors(DisplayHandle, Root, True, &MonitorCount);

    if (Monitors == NULL || MonitorCount <= 0) {
        printf("No active X11 monitors found.\n");
        if (Monitors != NULL) {
            XRRFreeMonitors(Monitors);
        }
        return;
    }

    for (int Index = 0; Index < MonitorCount; ++Index) {
        XRRMonitorInfo *Monitor = &Monitors[Index];
        printf("%d: %dx%d+%d+%d",
            Index + 1,
            Monitor->width,
            Monitor->height,
            Monitor->x,
            Monitor->y);

        if (Monitor->name != None) {
            char *Name = XGetAtomName(DisplayHandle, Monitor->name);
            if (Name != NULL) {
                printf(" [%s]", Name);
                XFree(Name);
            }
        }
        printf("\n");
    }

    XRRFreeMonitors(Monitors);
}

static enum AVPixelFormat getXImagePixelFormat(const XImage *Image)
{
    if (Image->bits_per_pixel == 24) {
        if (Image->red_mask == 0xff0000 && Image->green_mask == 0x00ff00 && Image->blue_mask == 0x0000ff) {
            return AV_PIX_FMT_BGR24;
        }
        if (Image->red_mask == 0x0000ff && Image->green_mask == 0x00ff00 && Image->blue_mask == 0xff0000) {
            return AV_PIX_FMT_RGB24;
        }
    }

    if (Image->bits_per_pixel == 32) {
        if (Image->red_mask == 0xff0000 && Image->green_mask == 0x00ff00 && Image->blue_mask == 0x0000ff) {
            return AV_PIX_FMT_0RGB32;
        }
        if (Image->red_mask == 0x0000ff && Image->green_mask == 0x00ff00 && Image->blue_mask == 0xff0000) {
            return AV_PIX_FMT_BGR0;
        }
    }

    return AV_PIX_FMT_NONE;
}

static bool initializeX11Capture(RecorderState *State)
{
    X11Capture *Capture = &State->Capture;
    memset(Capture, 0, sizeof(*Capture));

    Capture->DisplayHandle = State->DisplayHandle;
    Capture->Drawable = State->CaptureMode == CaptureWindow ? State->SelectedWindow : State->RootWindow;
    Capture->SourceX = State->CaptureMode == CaptureWindow ? 0 : State->CaptureX;
    Capture->SourceY = State->CaptureMode == CaptureWindow ? 0 : State->CaptureY;
    Capture->Width = State->CaptureWidth;
    Capture->Height = State->CaptureHeight;

    if (!XShmQueryExtension(State->DisplayHandle)) {
        fprintf(stderr, "XShm extension is unavailable.\n");
        return false;
    }

    int Screen = DefaultScreen(State->DisplayHandle);
    Capture->Image = XShmCreateImage(
        State->DisplayHandle,
        DefaultVisual(State->DisplayHandle, Screen),
        DefaultDepth(State->DisplayHandle, Screen),
        ZPixmap,
        NULL,
        &Capture->ShmInfo,
        Capture->Width,
        Capture->Height
    );

    if (Capture->Image == NULL) {
        fprintf(stderr, "XShmCreateImage failed.\n");
        return false;
    }

    int ShmSize = Capture->Image->bytes_per_line * Capture->Image->height;
    Capture->ShmInfo.shmid = shmget(IPC_PRIVATE, ShmSize, IPC_CREAT | 0600);
    if (Capture->ShmInfo.shmid < 0) {
        perror("shmget");
        XDestroyImage(Capture->Image);
        Capture->Image = NULL;
        return false;
    }

    Capture->ShmInfo.shmaddr = shmat(Capture->ShmInfo.shmid, NULL, 0);
    if (Capture->ShmInfo.shmaddr == (char *)-1) {
        perror("shmat");
        shmctl(Capture->ShmInfo.shmid, IPC_RMID, NULL);
        XDestroyImage(Capture->Image);
        Capture->Image = NULL;
        return false;
    }

    Capture->Image->data = Capture->ShmInfo.shmaddr;
    Capture->ShmInfo.readOnly = False;

    if (!XShmAttach(State->DisplayHandle, &Capture->ShmInfo)) {
        fprintf(stderr, "XShmAttach failed.\n");
        shmdt(Capture->ShmInfo.shmaddr);
        shmctl(Capture->ShmInfo.shmid, IPC_RMID, NULL);
        XDestroyImage(Capture->Image);
        Capture->Image = NULL;
        return false;
    }

    XSync(State->DisplayHandle, False);
    shmctl(Capture->ShmInfo.shmid, IPC_RMID, NULL);
    Capture->Attached = true;
    Capture->PixelFormat = getXImagePixelFormat(Capture->Image);

    if (Capture->PixelFormat == AV_PIX_FMT_NONE) {
        fprintf(stderr,
            "Unsupported X11 pixel format: bpp=%d r=0x%lx g=0x%lx b=0x%lx\n",
            Capture->Image->bits_per_pixel,
            Capture->Image->red_mask,
            Capture->Image->green_mask,
            Capture->Image->blue_mask);
        return false;
    }

    printf("X11 capture: %ux%u, source pixel format %s\n",
        Capture->Width,
        Capture->Height,
        av_get_pix_fmt_name(Capture->PixelFormat));

    return true;
}

static void destroyX11Capture(X11Capture *Capture)
{
    if (Capture->Attached) {
        XShmDetach(Capture->DisplayHandle, &Capture->ShmInfo);
        XSync(Capture->DisplayHandle, False);
    }
    Capture->Attached = false;

    if (Capture->Image != NULL) {
        Capture->Image->data = NULL;
        XDestroyImage(Capture->Image);
        Capture->Image = NULL;
    }

    if (Capture->ShmInfo.shmaddr != NULL && Capture->ShmInfo.shmaddr != (char *)-1) {
        shmdt(Capture->ShmInfo.shmaddr);
        Capture->ShmInfo.shmaddr = NULL;
    }
}

static bool captureFrame(RecorderState *State)
{
    X11Capture *Capture = &State->Capture;

    if (!XShmGetImage(
        State->DisplayHandle,
        Capture->Drawable,
        Capture->Image,
        Capture->SourceX,
        Capture->SourceY,
        AllPlanes
    )) {
        fprintf(stderr, "XShmGetImage failed.\n");
        return false;
    }

    return true;
}

static uint8_t clampByte(int Value)
{
    if (Value < 0) return 0;
    if (Value > 255) return 255;
    return (uint8_t)Value;
}

static void bgr0ToYuv(uint8_t B, uint8_t G, uint8_t R, uint8_t *Y, uint8_t *U, uint8_t *V)
{
    int LumaValue = (66 * R + 129 * G + 25 * B + 128) >> 8;
    int UValue = (-38 * R - 74 * G + 112 * B + 128) >> 8;
    int VValue = (112 * R - 94 * G - 18 * B + 128) >> 8;
    *Y = clampByte(LumaValue + 16);
    *U = clampByte(UValue + 128);
    *V = clampByte(VValue + 128);
}

static bool convertBgr0Frame(const AVFrame *Source, AVFrame *Destination)
{
    unsigned Width = (unsigned)Source->width;
    unsigned Height = (unsigned)Source->height;

    for (unsigned Y = 0; Y < Height; ++Y) {
        const uint8_t *SourceRow = Source->data[0] + Y * Source->linesize[0];
        uint8_t *YRow = Destination->data[0] + Y * Destination->linesize[0];

        for (unsigned X = 0; X < Width; ++X) {
            const uint8_t *Pixel = SourceRow + X * 4;
            uint8_t PixelY;
            uint8_t PixelU;
            uint8_t PixelV;
            bgr0ToYuv(Pixel[0], Pixel[1], Pixel[2], &PixelY, &PixelU, &PixelV);
            YRow[X] = PixelY;
        }
    }

    unsigned ChromaWidth = (Width + 1) >> 1;
    unsigned ChromaHeight = (Height + 1) >> 1;

    for (unsigned Y = 0; Y < ChromaHeight; ++Y) {
        unsigned SourceY0 = Y * 2;
        unsigned SourceY1 = SourceY0 + 1 < Height ? SourceY0 + 1 : SourceY0;

        const uint8_t *Row0 = Source->data[0] + SourceY0 * Source->linesize[0];
        const uint8_t *Row1 = Source->data[0] + SourceY1 * Source->linesize[0];

        uint8_t *UorUV = Destination->data[1] + Y * Destination->linesize[1];
        uint8_t *VPlane = Destination->data[2] != NULL ? Destination->data[2] + Y * Destination->linesize[2] : NULL;

        for (unsigned X = 0; X < ChromaWidth; ++X) {
            unsigned SourceX0 = X * 2;
            unsigned SourceX1 = SourceX0 + 1 < Width ? SourceX0 + 1 : SourceX0;

            const uint8_t *P00 = Row0 + SourceX0 * 4;
            const uint8_t *P01 = Row0 + SourceX1 * 4;
            const uint8_t *P10 = Row1 + SourceX0 * 4;
            const uint8_t *P11 = Row1 + SourceX1 * 4;

            uint8_t Y0, U0, V0;
            uint8_t Y1, U1, V1;
            uint8_t Y2, U2, V2;
            uint8_t Y3, U3, V3;
            bgr0ToYuv(P00[0], P00[1], P00[2], &Y0, &U0, &V0);
            bgr0ToYuv(P01[0], P01[1], P01[2], &Y1, &U1, &V1);
            bgr0ToYuv(P10[0], P10[1], P10[2], &Y2, &U2, &V2);
            bgr0ToYuv(P11[0], P11[1], P11[2], &Y3, &U3, &V3);

            uint8_t U = (uint8_t)((U0 + U1 + U2 + U3 + 2) >> 2);
            uint8_t V = (uint8_t)((V0 + V1 + V2 + V3 + 2) >> 2);

            if (Destination->format == AV_PIX_FMT_YUV420P) {
                UorUV[X] = U;
                VPlane[X] = V;
            } else if (Destination->format == AV_PIX_FMT_NV12) {
                UorUV[X * 2] = U;
                UorUV[X * 2 + 1] = V;
            }
        }
    }

    return Destination->format == AV_PIX_FMT_YUV420P || Destination->format == AV_PIX_FMT_NV12;
}

static enum AVPixelFormat chooseSoftwarePixelFormat(const AVCodec *Encoder)
{
    const enum AVPixelFormat *SupportedFormats = NULL;
    int SupportedFormatCount = 0;

    int Result = avcodec_get_supported_config(
        NULL,
        Encoder,
        AV_CODEC_CONFIG_PIX_FORMAT,
        0,
        (const void **)&SupportedFormats,
        &SupportedFormatCount
    );

    if (Result < 0 || SupportedFormats == NULL || SupportedFormatCount <= 0) {
        return AV_PIX_FMT_YUV420P;
    }

    static const enum AVPixelFormat Preferred[] = {
        AV_PIX_FMT_YUV420P,
        AV_PIX_FMT_NV12
    };

    for (size_t PreferredIndex = 0; PreferredIndex < sizeof(Preferred) / sizeof(Preferred[0]); ++PreferredIndex) {
        for (int FormatIndex = 0; FormatIndex < SupportedFormatCount; ++FormatIndex) {
            if (SupportedFormats[FormatIndex] == Preferred[PreferredIndex]) {
                return SupportedFormats[FormatIndex];
            }
        }
    }

    return AV_PIX_FMT_NONE;
}

static bool codecNameUsesVaapi(const char *CodecName)
{
    size_t Length = strlen(CodecName);
    return Length >= 6 && strcmp(CodecName + Length - 6, "_vaapi") == 0;
}

static bool createVaapiDevice(VideoWorker *Worker)
{
    static const char *DevicePaths[] = {
        "/dev/dri/card0",
        "/dev/dri/card1",
        "/dev/dri/card2",
        "/dev/dri/renderD128",
        "/dev/dri/renderD129",
        "/dev/dri/renderD130",
        "/dev/dri/renderD131"
    };

    int LastResult = AVERROR(EINVAL);

    for (size_t DeviceIndex = 0; DeviceIndex < sizeof(DevicePaths) / sizeof(DevicePaths[0]); ++DeviceIndex) {
        if (access(DevicePaths[DeviceIndex], R_OK | W_OK) != 0) {
            continue;
        }

        AVDictionary *Options = NULL;
        int Result = av_dict_set(&Options, "connection_type", "drm", 0);

        if (Result < 0) {
            av_dict_free(&Options);
            LastResult = Result;
            continue;
        }

        Result = av_hwdevice_ctx_create(
            &Worker->HwDevice,
            AV_HWDEVICE_TYPE_VAAPI,
            DevicePaths[DeviceIndex],
            Options,
            0
        );

        av_dict_free(&Options);

        if (Result >= 0) {
            printf("VA-API device: %s\n", DevicePaths[DeviceIndex]);
            break;
        }

        LastResult = Result;
        Worker->HwDevice = NULL;
    }

    if (Worker->HwDevice == NULL) {
        printError("av_hwdevice_ctx_create(VAAPI/DRM)", LastResult);
        return false;
    }

    Worker->HwFrames = av_hwframe_ctx_alloc(Worker->HwDevice);
    if (Worker->HwFrames == NULL) {
        return false;
    }

    AVHWFramesContext *FramesContext = (AVHWFramesContext *)Worker->HwFrames->data;
    FramesContext->format = AV_PIX_FMT_VAAPI;
    FramesContext->sw_format = AV_PIX_FMT_NV12;
    FramesContext->width = Worker->Encoder->width;
    FramesContext->height = Worker->Encoder->height;
    FramesContext->initial_pool_size = 8;

    int Result = av_hwframe_ctx_init(Worker->HwFrames);
    if (Result < 0) {
        printError("av_hwframe_ctx_init", Result);
        return false;
    }

    return true;
}

static int encoderPriority(const AVCodec *Encoder)
{
    static const char *Preferred[] = {
        "h264_vaapi",
        "libx264",
        "h264_nvenc",
        "h264_qsv",
        "h264_amf",
        "hevc_vaapi",
        "libx265",
        "mpeg4"
    };

    for (size_t Index = 0; Index < sizeof(Preferred) / sizeof(Preferred[0]); ++Index) {
        if (strcmp(Encoder->name, Preferred[Index]) == 0) {
            return (int)Index;
        }
    }

    return 1000;
}

static bool tryVideoEncoder(RecorderState *State, const AVCodec *Encoder)
{
    VideoWorker *Worker = &State->Video;

    if (Encoder == NULL || Encoder->type != AVMEDIA_TYPE_VIDEO) {
        return false;
    }

    AVCodecContext *EncoderContext = avcodec_alloc_context3(Encoder);
    if (EncoderContext == NULL) {
        return false;
    }

    EncoderContext->width = (int)State->CaptureWidth;
    EncoderContext->height = (int)State->CaptureHeight;
    EncoderContext->time_base = (AVRational){1, State->FrameRate};
    EncoderContext->framerate = (AVRational){State->FrameRate, 1};
    EncoderContext->bit_rate = State->VideoBitrate;
    EncoderContext->max_b_frames = 0;

    if (State->Output->oformat->flags & AVFMT_GLOBALHEADER) {
        EncoderContext->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
    }

    bool UseVaapi = codecNameUsesVaapi(Encoder->name);
    Worker->Encoder = EncoderContext;
    Worker->UseVaapi = UseVaapi;
    Worker->HwDevice = NULL;
    Worker->HwFrames = NULL;

    if (UseVaapi) {
        Worker->Encoder->pix_fmt = AV_PIX_FMT_VAAPI;

        if (!createVaapiDevice(Worker)) {
            av_buffer_unref(&Worker->HwFrames);
            av_buffer_unref(&Worker->HwDevice);
            avcodec_free_context(&Worker->Encoder);
            return false;
        }

        Worker->Encoder->hw_frames_ctx = av_buffer_ref(Worker->HwFrames);
        if (Worker->Encoder->hw_frames_ctx == NULL) {
            av_buffer_unref(&Worker->HwFrames);
            av_buffer_unref(&Worker->HwDevice);
            avcodec_free_context(&Worker->Encoder);
            return false;
        }

        Worker->SoftwarePixelFormat = AV_PIX_FMT_NV12;
    } else {
        Worker->SoftwarePixelFormat = chooseSoftwarePixelFormat(Encoder);
        if (Worker->SoftwarePixelFormat == AV_PIX_FMT_NONE) {
            avcodec_free_context(&Worker->Encoder);
            return false;
        }
        Worker->Encoder->pix_fmt = Worker->SoftwarePixelFormat;
    }

    printf("Trying video encoder: %s\n", Encoder->name);

    int Result = avcodec_open2(Worker->Encoder, Encoder, NULL);
    if (Result < 0) {
        fprintf(stderr, "Video encoder '%s' failed to initialize.\n", Encoder->name);
        printError("avcodec_open2(video)", Result);
        av_buffer_unref(&Worker->HwFrames);
        av_buffer_unref(&Worker->HwDevice);
        avcodec_free_context(&Worker->Encoder);
        return false;
    }

    if (avformat_query_codec(State->Output->oformat, Encoder->id, FF_COMPLIANCE_NORMAL) <= 0) {
        fprintf(stderr, "Video encoder '%s' is not accepted by output format '%s'.\n", Encoder->name, State->Output->oformat->name);
        avcodec_free_context(&Worker->Encoder);
        av_buffer_unref(&Worker->HwFrames);
        av_buffer_unref(&Worker->HwDevice);
        return false;
    }

    Worker->Stream = avformat_new_stream(State->Output, NULL);
    if (Worker->Stream == NULL) {
        avcodec_free_context(&Worker->Encoder);
        av_buffer_unref(&Worker->HwFrames);
        av_buffer_unref(&Worker->HwDevice);
        return false;
    }

    Worker->Stream->time_base = Worker->Encoder->time_base;
    Result = avcodec_parameters_from_context(Worker->Stream->codecpar, Worker->Encoder);
    if (Result < 0) {
        printError("avcodec_parameters_from_context(video)", Result);
        return false;
    }
    printf("Video encoder: %s%s\n", Encoder->name, UseVaapi ? " (VA-API)" : "");
    return true;
}

static bool initializeVideoEncoder(RecorderState *State)
{
    VideoWorker *Worker = &State->Video;

    if (State->CodecExplicit) {
        const AVCodec *Encoder = avcodec_find_encoder_by_name(State->CodecName);

        if (Encoder == NULL) {
            fprintf(stderr, "Video encoder '%s' is unavailable.\n", State->CodecName);
            return false;
        }

        if (!tryVideoEncoder(State, Encoder)) {
            fprintf(stderr, "Video encoder '%s' could not be initialized.\n", State->CodecName);
            return false;
        }
    } else {
        fprintf(stderr, "Available video encoders in this build:\n");
        {
            void *ListIterator = NULL;
            const AVCodec *ListEncoder;
            while ((ListEncoder = av_codec_iterate(&ListIterator)) != NULL) {
                if (av_codec_is_encoder(ListEncoder) && ListEncoder->type == AVMEDIA_TYPE_VIDEO) {
                    fprintf(stderr, "  %s\n", ListEncoder->name);
                }
            }
        }

        void *Iterator = NULL;
        const AVCodec *Encoder;

        while ((Encoder = av_codec_iterate(&Iterator)) != NULL && Worker->Encoder == NULL) {
            if (!av_codec_is_encoder(Encoder) || Encoder->type != AVMEDIA_TYPE_VIDEO) {
                continue;
            }

            if (encoderPriority(Encoder) >= 1000) {
                continue;
            }

            if (!tryVideoEncoder(State, Encoder)) {
                fprintf(stderr, "Trying next video encoder.\n");
            }
        }

        if (Worker->Encoder == NULL) {
            Iterator = NULL;

            while ((Encoder = av_codec_iterate(&Iterator)) != NULL && Worker->Encoder == NULL) {
                if (!av_codec_is_encoder(Encoder) || Encoder->type != AVMEDIA_TYPE_VIDEO) {
                    continue;
                }

                if (encoderPriority(Encoder) < 1000) {
                    continue;
                }

                if (!tryVideoEncoder(State, Encoder)) {
                    fprintf(stderr, "Trying next video encoder.\n");
                }
            }
        }

        if (Worker->Encoder == NULL) {
            fprintf(stderr, "No usable video encoder is available in this build.\n");
            return false;
        }
    }

    if (Worker->Stream == NULL || Worker->Encoder == NULL) {
        return false;
    }

    return true;
}

static bool initializeAudioEncoder(AudioWorker *Worker, RecorderState *State)
{
    const AVCodec *Encoder = avcodec_find_encoder(AV_CODEC_ID_AAC);
    if (Encoder == NULL) {
        fprintf(stderr, "AAC encoder is unavailable.\n");
        return false;
    }

    Worker->Encoder = avcodec_alloc_context3(Encoder);
    if (Worker->Encoder == NULL) {
        return false;
    }

    Worker->Encoder->sample_rate = AudioSampleRate;
    Worker->Encoder->bit_rate = State->AudioBitrate;

    const enum AVSampleFormat *SupportedSampleFormats = NULL;
    int SupportedSampleFormatCount = 0;

    int ConfigResult = avcodec_get_supported_config(
        NULL,
        Encoder,
        AV_CODEC_CONFIG_SAMPLE_FORMAT,
        0,
        (const void **)&SupportedSampleFormats,
        &SupportedSampleFormatCount
    );

    Worker->Encoder->sample_fmt = AV_SAMPLE_FMT_FLTP;

    if (ConfigResult >= 0 &&
        SupportedSampleFormats != NULL &&
        SupportedSampleFormatCount > 0) {
        bool SupportsFloatPlanar = false;
        bool SupportsS16Planar = false;
        bool SupportsS16 = false;

        for (int Index = 0; Index < SupportedSampleFormatCount; ++Index) {
            if (SupportedSampleFormats[Index] == AV_SAMPLE_FMT_FLTP) {
                SupportsFloatPlanar = true;
            } else if (SupportedSampleFormats[Index] == AV_SAMPLE_FMT_S16P) {
                SupportsS16Planar = true;
            } else if (SupportedSampleFormats[Index] == AV_SAMPLE_FMT_S16) {
                SupportsS16 = true;
            }
        }

        if (SupportsFloatPlanar) {
            Worker->Encoder->sample_fmt = AV_SAMPLE_FMT_FLTP;
        } else if (SupportsS16Planar) {
            Worker->Encoder->sample_fmt = AV_SAMPLE_FMT_S16P;
        } else if (SupportsS16) {
            Worker->Encoder->sample_fmt = AV_SAMPLE_FMT_S16;
        } else {
            Worker->Encoder->sample_fmt = SupportedSampleFormats[0];
        }
    }

    av_channel_layout_default(&Worker->Encoder->ch_layout, AudioChannels);

    if (State->Output->oformat->flags & AVFMT_GLOBALHEADER) {
        Worker->Encoder->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
    }

    int Result = avcodec_open2(Worker->Encoder, Encoder, NULL);
    if (Result < 0) {
        printError("avcodec_open2(aac)", Result);
        return false;
    }

    Worker->Stream = avformat_new_stream(State->Output, NULL);
    if (Worker->Stream == NULL) {
        return false;
    }

    Worker->Stream->time_base = (AVRational){1, AudioSampleRate};
    Result = avcodec_parameters_from_context(Worker->Stream->codecpar, Worker->Encoder);
    if (Result < 0) {
        printError("avcodec_parameters_from_context(audio)", Result);
        return false;
    }

    return true;
}

static bool convertAudioSamples(const int16_t *Input, AVFrame *OutputFrame, int SampleCount)
{
    if (OutputFrame->ch_layout.nb_channels != AudioChannels) {
        return false;
    }

    switch (OutputFrame->format) {
        case AV_SAMPLE_FMT_S16:
            memcpy(OutputFrame->data[0], Input, (size_t)SampleCount * AudioChannels * sizeof(int16_t));
            return true;

        case AV_SAMPLE_FMT_S16P:
            for (int Sample = 0; Sample < SampleCount; ++Sample) {
                ((int16_t *)OutputFrame->data[0])[Sample] = Input[Sample * AudioChannels];
                ((int16_t *)OutputFrame->data[1])[Sample] = Input[Sample * AudioChannels + 1];
            }
            return true;

        case AV_SAMPLE_FMT_FLT:
            for (int Sample = 0; Sample < SampleCount; ++Sample) {
                ((float *)OutputFrame->data[0])[Sample * AudioChannels] =
                    (float)Input[Sample * AudioChannels] / 32768.0f;
                ((float *)OutputFrame->data[0])[Sample * AudioChannels + 1] =
                    (float)Input[Sample * AudioChannels + 1] / 32768.0f;
            }
            return true;

        case AV_SAMPLE_FMT_FLTP:
            for (int Sample = 0; Sample < SampleCount; ++Sample) {
                ((float *)OutputFrame->data[0])[Sample] =
                    (float)Input[Sample * AudioChannels] / 32768.0f;
                ((float *)OutputFrame->data[1])[Sample] =
                    (float)Input[Sample * AudioChannels + 1] / 32768.0f;
            }
            return true;

        default:
            fprintf(stderr, "Unsupported AAC sample format: %s\n", av_get_sample_fmt_name(OutputFrame->format));
            return false;
    }
}

static bool openAudioDevice(AudioWorker *Worker)
{
    pa_sample_spec SampleSpec = {
        .format = PA_SAMPLE_S16LE,
        .rate = AudioSampleRate,
        .channels = AudioChannels
    };

    int Error = 0;
    Worker->Pulse = pa_simple_new(
        NULL,
        "screenRecorder",
        PA_STREAM_RECORD,
        Worker->DeviceName,
        Worker->Description,
        &SampleSpec,
        NULL,
        NULL,
        &Error
    );

    if (Worker->Pulse == NULL) {
        fprintf(stderr, "%s: %s\n", Worker->Description, pa_strerror(Error));
        return false;
    }

    return true;
}

static bool writePacket(RecorderState *State, AVPacket *Packet)
{
    int Result;
    pthread_mutex_lock(&State->MuxMutex);
    Result = av_interleaved_write_frame(State->Output, Packet);
    pthread_mutex_unlock(&State->MuxMutex);

    if (Result < 0) {
        printError("av_interleaved_write_frame", Result);
        requestFatal(State);
        return false;
    }

    return true;
}

static bool encodeVideoFrame(RecorderState *State, AVFrame *SoftwareFrame, int64_t FramePts)
{
    VideoWorker *Worker = &State->Video;
    AVFrame *ConvertedFrame = NULL;
    AVFrame *EncodeFrame = NULL;
    AVFrame *HardwareFrame = NULL;
    int Result;

    ConvertedFrame = av_frame_alloc();
    if (ConvertedFrame == NULL) {
        return false;
    }

    ConvertedFrame->format = Worker->SoftwarePixelFormat;
    ConvertedFrame->width = Worker->Encoder->width;
    ConvertedFrame->height = Worker->Encoder->height;
    Result = av_frame_get_buffer(ConvertedFrame, 32);
    if (Result < 0) {
        printError("av_frame_get_buffer(video)", Result);
        av_frame_free(&ConvertedFrame);
        return false;
    }

    if (!convertBgr0Frame(SoftwareFrame, ConvertedFrame)) {
        fprintf(stderr, "Could not convert captured video frame.\n");
        av_frame_free(&ConvertedFrame);
        return false;
    }

    ConvertedFrame->pts = FramePts;

    if (Worker->UseVaapi) {
        HardwareFrame = av_frame_alloc();
        if (HardwareFrame == NULL) {
            av_frame_free(&ConvertedFrame);
            return false;
        }

        HardwareFrame->format = AV_PIX_FMT_VAAPI;
        HardwareFrame->width = Worker->Encoder->width;
        HardwareFrame->height = Worker->Encoder->height;
        HardwareFrame->pts = ConvertedFrame->pts;

        Result = av_hwframe_get_buffer(Worker->HwFrames, HardwareFrame, 0);
        if (Result < 0) {
            printError("av_hwframe_get_buffer", Result);
            av_frame_free(&HardwareFrame);
            av_frame_free(&ConvertedFrame);
            return false;
        }

        Result = av_hwframe_transfer_data(HardwareFrame, ConvertedFrame, 0);
        av_frame_free(&ConvertedFrame);
        ConvertedFrame = NULL;

        if (Result < 0) {
            printError("av_hwframe_transfer_data", Result);
            av_frame_free(&HardwareFrame);
            return false;
        }

        EncodeFrame = HardwareFrame;
    } else {
        EncodeFrame = ConvertedFrame;
    }

    Result = avcodec_send_frame(Worker->Encoder, EncodeFrame);
    if (Result < 0) {
        printError("avcodec_send_frame(video)", Result);
        av_frame_free(&HardwareFrame);
        av_frame_free(&ConvertedFrame);
        return false;
    }

    AVPacket *Packet = av_packet_alloc();
    if (Packet == NULL) {
        av_frame_free(&HardwareFrame);
        av_frame_free(&ConvertedFrame);
        return false;
    }

    while ((Result = avcodec_receive_packet(Worker->Encoder, Packet)) >= 0) {
        av_packet_rescale_ts(Packet, Worker->Encoder->time_base, Worker->Stream->time_base);
        Packet->stream_index = Worker->Stream->index;

        if (!writePacket(State, Packet)) {
            av_packet_free(&Packet);
            av_frame_free(&HardwareFrame);
            av_frame_free(&ConvertedFrame);
            return false;
        }

        av_packet_unref(Packet);
    }

    av_packet_free(&Packet);
    av_frame_free(&HardwareFrame);
    av_frame_free(&ConvertedFrame);

    if (Result != AVERROR(EAGAIN) && Result != AVERROR_EOF) {
        printError("avcodec_receive_packet(video)", Result);
        return false;
    }

    return true;
}

static bool flushVideo(RecorderState *State)
{
    VideoWorker *Worker = &State->Video;
    int Result = avcodec_send_frame(Worker->Encoder, NULL);
    if (Result < 0 && Result != AVERROR_EOF) {
        printError("avcodec_send_frame(video flush)", Result);
        return false;
    }

    AVPacket *Packet = av_packet_alloc();
    if (Packet == NULL) {
        return false;
    }

    while ((Result = avcodec_receive_packet(Worker->Encoder, Packet)) >= 0) {
        av_packet_rescale_ts(Packet, Worker->Encoder->time_base, Worker->Stream->time_base);
        Packet->stream_index = Worker->Stream->index;
        if (!writePacket(State, Packet)) {
            av_packet_free(&Packet);
            return false;
        }
        av_packet_unref(Packet);
    }

    av_packet_free(&Packet);
    return Result == AVERROR_EOF || Result == AVERROR(EAGAIN);
}

static bool encodeAudioFrame(RecorderState *State, AudioWorker *Worker, AVFrame *Frame)
{
    int Result = avcodec_send_frame(Worker->Encoder, Frame);
    if (Result < 0) {
        printError("avcodec_send_frame(audio)", Result);
        return false;
    }

    AVPacket *Packet = av_packet_alloc();
    if (Packet == NULL) {
        return false;
    }

    while ((Result = avcodec_receive_packet(Worker->Encoder, Packet)) >= 0) {
        av_packet_rescale_ts(Packet, Worker->Encoder->time_base, Worker->Stream->time_base);
        Packet->stream_index = Worker->Stream->index;
        if (!writePacket(State, Packet)) {
            av_packet_free(&Packet);
            return false;
        }
        av_packet_unref(Packet);
    }

    av_packet_free(&Packet);

    if (Result != AVERROR(EAGAIN) && Result != AVERROR_EOF) {
        printError("avcodec_receive_packet(audio)", Result);
        return false;
    }

    return true;
}

static bool flushAudio(RecorderState *State, AudioWorker *Worker)
{
    int Result = avcodec_send_frame(Worker->Encoder, NULL);
    if (Result < 0 && Result != AVERROR_EOF) {
        printError("avcodec_send_frame(audio flush)", Result);
        return false;
    }

    AVPacket *Packet = av_packet_alloc();
    if (Packet == NULL) {
        return false;
    }

    while ((Result = avcodec_receive_packet(Worker->Encoder, Packet)) >= 0) {
        av_packet_rescale_ts(Packet, Worker->Encoder->time_base, Worker->Stream->time_base);
        Packet->stream_index = Worker->Stream->index;
        if (!writePacket(State, Packet)) {
            av_packet_free(&Packet);
            return false;
        }
        av_packet_unref(Packet);
    }

    av_packet_free(&Packet);
    return Result == AVERROR(EAGAIN) || Result == AVERROR_EOF;
}

static void *videoThreadMain(void *Opaque)
{
    RecorderState *State = Opaque;
    VideoWorker *Worker = &State->Video;

    AVFrame *SourceFrame = av_frame_alloc();
    if (SourceFrame == NULL) {
        requestFatal(State);
        return NULL;
    }

    SourceFrame->format = Worker->SourcePixelFormat;
    SourceFrame->width = (int)State->CaptureWidth;
    SourceFrame->height = (int)State->CaptureHeight;

    struct timespec StartTime;
    clock_gettime(CLOCK_MONOTONIC, &StartTime);

    uint64_t FrameIndex = 0;

    while (!atomic_load_explicit(&State->StopRequested, memory_order_relaxed)) {
        uint64_t TargetNs = (uint64_t)StartTime.tv_sec * 1000000000ULL + (uint64_t)StartTime.tv_nsec
            + FrameIndex * (1000000000ULL / (unsigned)State->FrameRate);

        struct timespec Now;
        clock_gettime(CLOCK_MONOTONIC, &Now);
        uint64_t NowNs = (uint64_t)Now.tv_sec * 1000000000ULL + (uint64_t)Now.tv_nsec;

        if (TargetNs > NowNs) {
            struct timespec SleepTime = {
                .tv_sec = (time_t)((TargetNs - NowNs) / 1000000000ULL),
                .tv_nsec = (long)((TargetNs - NowNs) % 1000000000ULL)
            };
            nanosleep(&SleepTime, NULL);
        }

        if (!captureFrame(State)) {
            requestFatal(State);
            break;
        }

        struct timespec FrameTime;
        clock_gettime(CLOCK_MONOTONIC, &FrameTime);
        uint64_t FrameNs = ((uint64_t)FrameTime.tv_sec * 1000000000ULL) + (uint64_t)FrameTime.tv_nsec;
        uint64_t StartNs = ((uint64_t)StartTime.tv_sec * 1000000000ULL) + (uint64_t)StartTime.tv_nsec;
        int64_t FramePts = av_rescale_q((int64_t)(FrameNs - StartNs), (AVRational){1, 1000000000}, Worker->Encoder->time_base);
        if (FramePts < Worker->NextPts) {
            FramePts = Worker->NextPts;
        }
        Worker->NextPts = FramePts + 1;

        SourceFrame->data[0] = (uint8_t *)State->Capture.Image->data;
        SourceFrame->linesize[0] = State->Capture.Image->bytes_per_line;

        if (!encodeVideoFrame(State, SourceFrame, FramePts)) {
            requestFatal(State);
            break;
        }

        ++FrameIndex;
    }

    flushVideo(State);
    av_frame_free(&SourceFrame);
    return NULL;
}

static void *audioThreadMain(void *Opaque)
{
    AudioWorker *Worker = Opaque;
    RecorderState *State = Worker->State;

    if (!openAudioDevice(Worker)) {
        requestFatal(State);
        return NULL;
    }

    AVFrame *InputFrame = av_frame_alloc();
    AVFrame *OutputFrame = av_frame_alloc();
    if (InputFrame == NULL || OutputFrame == NULL) {
        av_frame_free(&InputFrame);
        av_frame_free(&OutputFrame);
        requestFatal(State);
        return NULL;
    }

    InputFrame->format = AV_SAMPLE_FMT_S16;
    InputFrame->sample_rate = AudioSampleRate;
    av_channel_layout_default(&InputFrame->ch_layout, AudioChannels);
    InputFrame->nb_samples = AudioFrameSamples;

    OutputFrame->format = Worker->Encoder->sample_fmt;
    OutputFrame->sample_rate = AudioSampleRate;
    av_channel_layout_copy(&OutputFrame->ch_layout, &Worker->Encoder->ch_layout);
    OutputFrame->nb_samples = AudioFrameSamples;

    if (av_frame_get_buffer(InputFrame, 0) < 0 || av_frame_get_buffer(OutputFrame, 0) < 0) {
        av_frame_free(&InputFrame);
        av_frame_free(&OutputFrame);
        requestFatal(State);
        return NULL;
    }

    while (!atomic_load_explicit(&State->StopRequested, memory_order_relaxed)) {
        int Error = 0;
        size_t Bytes = (size_t)AudioFrameSamples * AudioChannels * sizeof(int16_t);

        int Result = pa_simple_read(Worker->Pulse, InputFrame->data[0], Bytes, &Error);
        if (Result < 0) {
            fprintf(stderr, "%s: %s\n", Worker->Description, pa_strerror(Error));
            requestFatal(State);
            break;
        }

        OutputFrame->pts = Worker->NextPts;
        OutputFrame->nb_samples = InputFrame->nb_samples;

        if (!convertAudioSamples((const int16_t *)InputFrame->data[0], OutputFrame, InputFrame->nb_samples)) {
            fprintf(stderr, "%s: audio conversion failed.\n", Worker->Description);
            requestFatal(State);
            break;
        }

        Worker->NextPts += OutputFrame->nb_samples;

        if (!encodeAudioFrame(State, Worker, OutputFrame)) {
            requestFatal(State);
            break;
        }

        OutputFrame->nb_samples = AudioFrameSamples;
    }

    pa_simple_free(Worker->Pulse);
    Worker->Pulse = NULL;
    av_frame_free(&InputFrame);
    av_frame_free(&OutputFrame);
    flushAudio(State, Worker);
    return NULL;
}

static bool createDirectoryRecursive(const char *Path)
{
    char Buffer[PATH_MAX];
    size_t Length = strlen(Path);
    if (Length == 0 || Length >= sizeof(Buffer)) {
        return false;
    }

    memcpy(Buffer, Path, Length + 1);
    if (Buffer[Length - 1] == '/') {
        Buffer[Length - 1] = '\0';
    }

    for (char *Cursor = Buffer + 1; *Cursor != '\0'; ++Cursor) {
        if (*Cursor != '/') {
            continue;
        }
        *Cursor = '\0';
        if (mkdir(Buffer, 0755) != 0 && errno != EEXIST) {
            return false;
        }
        *Cursor = '/';
    }

    return mkdir(Buffer, 0755) == 0 || errno == EEXIST;
}

static bool createOutputPath(RecorderState *State)
{
    if (!createDirectoryRecursive(State->OutputDirectory)) {
        fprintf(stderr, "Could not create output directory '%s': %s\n", State->OutputDirectory, strerror(errno));
        return false;
    }

    time_t Now = time(NULL);
    struct tm LocalTime;
    localtime_r(&Now, &LocalTime);

    char Timestamp[64];
    strftime(Timestamp, sizeof(Timestamp), "%Y-%m-%d-%H-%M-%S", &LocalTime);

    int Written = snprintf(
        State->OutputPath,
        sizeof(State->OutputPath),
        "%s/recording-%s-%ld.mp4",
        State->OutputDirectory,
        Timestamp,
        (long)getpid()
    );

    return Written > 0 && (size_t)Written < sizeof(State->OutputPath);
}

static bool initializeOutput(RecorderState *State)
{
    int Result = avformat_alloc_output_context2(
        &State->Output,
        NULL,
        NULL,
        State->OutputPath
    );

    if (Result < 0 || State->Output == NULL) {
        printError("avformat_alloc_output_context2", Result < 0 ? Result : AVERROR(EINVAL));
        return false;
    }


    if (!(State->Output->oformat->flags & AVFMT_NOFILE)) {
        Result = avio_open(&State->Output->pb, State->OutputPath, AVIO_FLAG_WRITE);
        if (Result < 0) {
            printError("avio_open", Result);
            return false;
        }
    }

    if (!initializeVideoEncoder(State) ||
        !initializeAudioEncoder(&State->DesktopAudio, State) ||
        !initializeAudioEncoder(&State->Microphone, State)) {
        return false;
    }

    AVDictionary *Options = NULL;
    Result = avformat_write_header(State->Output, &Options);
    av_dict_free(&Options);

    if (Result < 0) {
        printError("avformat_write_header", Result);
        return false;
    }

    printf("Recording to: %s\n", State->OutputPath);
    printf("Desktop audio: separate track\n");
    printf("Microphone: separate track\n");
    printf("Press Ctrl+C to stop.\n");
    return true;
}

static void cleanupRecorder(RecorderState *State)
{
    destroyX11Capture(&State->Capture);

    av_buffer_unref(&State->Video.HwFrames);
    av_buffer_unref(&State->Video.HwDevice);
    avcodec_free_context(&State->Video.Encoder);

    if (State->DesktopAudio.Pulse != NULL) {
        pa_simple_free(State->DesktopAudio.Pulse);
    }
    if (State->Microphone.Pulse != NULL) {
        pa_simple_free(State->Microphone.Pulse);
    }

    avcodec_free_context(&State->DesktopAudio.Encoder);
    avcodec_free_context(&State->Microphone.Encoder);

    if (State->Output != NULL) {
        if (State->Output->pb != NULL) {
            avio_closep(&State->Output->pb);
        }
        avformat_free_context(State->Output);
        State->Output = NULL;
    }

    if (State->DisplayHandle != NULL) {
        XCloseDisplay(State->DisplayHandle);
        State->DisplayHandle = NULL;
    }
}

static void listCodecs(void)
{
    void *Iterator = NULL;
    const AVCodec *Codec;

    while ((Codec = av_codec_iterate(&Iterator)) != NULL) {
        if (!av_codec_is_encoder(Codec) || Codec->type != AVMEDIA_TYPE_VIDEO) {
            continue;
        }
        printf("%-24s %s\n", Codec->name, Codec->long_name != NULL ? Codec->long_name : "");
    }
}

static void printUsage(const char *ProgramName)
{
    printf("Usage: %s [options]\n\n", ProgramName);
    printf("With no capture-mode flag, the entire X11 screen is recorded.\n\n");
    printf("Capture modes:\n");
    printf("  --screen                 Record the entire X11 screen.\n");
    printf("  --window                 Click a window to record it.\n");
    printf("  --area                   Drag an area to record.\n");
    printf("  --monitor N              Record monitor N from RandR.\n\n");
    printf("Output and codec:\n");
    printf("  --output-dir DIR         Write recordings into DIR.\n");
    printf("  --codec CODEC            Use a specific video encoder.\n");
    printf("                           Missing CODEC prints this help.\n\n");
    printf("Information:\n");
    printf("  --list-monitors          List active X11 monitors.\n");
    printf("  --list-codecs            List video encoders compiled into the binary.\n");
    printf("  --help                   Show this help.\n\n");
    printf("Audio is always recorded as separate desktop and microphone tracks.\n");
}

static bool parseArguments(RecorderState *State, int Argc, char **Argv, bool *ListMonitors, bool *ListCodecs)
{
    bool CaptureModeExplicit = false;

    for (int Index = 1; Index < Argc; ++Index) {
        const char *Argument = Argv[Index];

        if (strcmp(Argument, "--help") == 0 || strcmp(Argument, "-h") == 0) {
            printUsage(Argv[0]);
            return false;
        }

        if (strcmp(Argument, "--screen") == 0 || strcmp(Argument, "--window") == 0 || strcmp(Argument, "--area") == 0) {
            if (CaptureModeExplicit) {
                fprintf(stderr, "Only one capture mode may be selected.\n");
                return false;
            }
            State->CaptureMode = strcmp(Argument, "--screen") == 0 ? CaptureScreen :
                strcmp(Argument, "--window") == 0 ? CaptureWindow : CaptureArea;
            CaptureModeExplicit = true;
            continue;
        }

        if (strcmp(Argument, "--monitor") == 0) {
            if (Index + 1 >= Argc) {
                printUsage(Argv[0]);
                return false;
            }
            if (CaptureModeExplicit) {
                fprintf(stderr, "Only one capture mode may be selected.\n");
                return false;
            }
            char *End = NULL;
            unsigned long Value = strtoul(Argv[++Index], &End, 10);
            if (End == Argv[Index] || *End != '\0' || Value == 0 || Value > UINT_MAX) {
                fprintf(stderr, "Invalid monitor number.\n");
                return false;
            }
            State->CaptureMode = CaptureMonitor;
            State->MonitorNumber = (unsigned)Value;
            CaptureModeExplicit = true;
            continue;
        }

        if (strcmp(Argument, "--output-dir") == 0) {
            if (Index + 1 >= Argc) {
                printUsage(Argv[0]);
                return false;
            }
            if (snprintf(State->OutputDirectory, sizeof(State->OutputDirectory), "%s", Argv[++Index]) >= (int)sizeof(State->OutputDirectory)) {
                fprintf(stderr, "Output directory path is too long.\n");
                return false;
            }
            continue;
        }

        if (strcmp(Argument, "--codec") == 0) {
            if (Index + 1 >= Argc || Argv[Index + 1][0] == '-') {
                printUsage(Argv[0]);
                return false;
            }
            if (snprintf(State->CodecName, sizeof(State->CodecName), "%s", Argv[++Index]) >= (int)sizeof(State->CodecName)) {
                fprintf(stderr, "Codec name is too long.\n");
                return false;
            }
            State->CodecExplicit = true;
            continue;
        }

        if (strcmp(Argument, "--list-monitors") == 0) {
            *ListMonitors = true;
            continue;
        }

        if (strcmp(Argument, "--list-codecs") == 0) {
            *ListCodecs = true;
            continue;
        }

        fprintf(stderr, "Unknown option: %s\n", Argument);
        printUsage(Argv[0]);
        return false;
    }

    return true;
}

int main(int Argc, char **Argv)
{
    RecorderState State = {0};
    State.CaptureMode = CaptureScreen;
    State.FrameRate = DefaultFrameRate;
    State.VideoBitrate = DefaultVideoBitrate;
    State.AudioBitrate = DefaultAudioBitrate;
    snprintf(State.OutputDirectory, sizeof(State.OutputDirectory), "%s", ".");
    atomic_init(&State.StopRequested, false);
    atomic_init(&State.FatalError, false);

    bool ListMonitorsRequested = false;
    bool ListCodecsRequested = false;

    if (!parseArguments(&State, Argc, Argv, &ListMonitorsRequested, &ListCodecsRequested)) {
        return 1;
    }

    if (ListMonitorsRequested || ListCodecsRequested) {
        if ((ListMonitorsRequested && ListCodecsRequested) || Argc != 2) {
            fprintf(stderr, "Information options cannot be combined with other options.\n");
            return 1;
        }

        if (ListMonitorsRequested) {
            Display *DisplayHandle = XOpenDisplay(NULL);
            if (DisplayHandle == NULL) {
                fprintf(stderr, "Could not open X11 display.\n");
                return 1;
            }
            listMonitors(DisplayHandle);
            XCloseDisplay(DisplayHandle);
            return 0;
        }

        listCodecs();
        return 0;
    }

    if (pthread_mutex_init(&State.MuxMutex, NULL) != 0) {
        fprintf(stderr, "Could not initialize mux mutex.\n");
        return 1;
    }

    State.DisplayHandle = XOpenDisplay(NULL);
    if (State.DisplayHandle == NULL) {
        fprintf(stderr, "Could not open X11 display.\n");
        pthread_mutex_destroy(&State.MuxMutex);
        return 1;
    }

    State.RootWindow = RootWindow(State.DisplayHandle, DefaultScreen(State.DisplayHandle));

    if (State.CaptureMode == CaptureWindow) {
        if (!selectWindow(&State)) {
            fprintf(stderr, "Window selection cancelled.\n");
            cleanupRecorder(&State);
            pthread_mutex_destroy(&State.MuxMutex);
            return 1;
        }
        printf("Selected window 0x%lx (%ux%u)\n",
            (unsigned long)State.SelectedWindow,
            State.CaptureWidth,
            State.CaptureHeight);
    } else if (State.CaptureMode == CaptureArea) {
        if (!selectArea(&State)) {
            fprintf(stderr, "Area selection cancelled.\n");
            cleanupRecorder(&State);
            pthread_mutex_destroy(&State.MuxMutex);
            return 1;
        }
        printf("Selected area %ux%u+%d+%d\n",
            State.CaptureWidth,
            State.CaptureHeight,
            State.CaptureX,
            State.CaptureY);
    } else if (State.CaptureMode == CaptureMonitor) {
        if (!getMonitorGeometry(&State, State.MonitorNumber)) {
            cleanupRecorder(&State);
            pthread_mutex_destroy(&State.MuxMutex);
            return 1;
        }
    } else {
        State.CaptureX = 0;
        State.CaptureY = 0;
        State.CaptureWidth = (unsigned)DisplayWidth(State.DisplayHandle, DefaultScreen(State.DisplayHandle)) & ~1U;
        State.CaptureHeight = (unsigned)DisplayHeight(State.DisplayHandle, DefaultScreen(State.DisplayHandle)) & ~1U;
    }

    if (State.CaptureWidth < 2 || State.CaptureHeight < 2) {
        fprintf(stderr, "Capture dimensions are invalid.\n");
        cleanupRecorder(&State);
        pthread_mutex_destroy(&State.MuxMutex);
        return 1;
    }

    if (!createOutputPath(&State)) {
        cleanupRecorder(&State);
        pthread_mutex_destroy(&State.MuxMutex);
        return 1;
    }

    if (!initializeX11Capture(&State)) {
        cleanupRecorder(&State);
        pthread_mutex_destroy(&State.MuxMutex);
        return 1;
    }

    State.Video.SourcePixelFormat = State.Capture.PixelFormat;
    State.DesktopAudio.State = &State;
    State.DesktopAudio.DeviceName = "@DEFAULT_MONITOR@";
    State.DesktopAudio.Description = "Desktop audio";
    State.Microphone.State = &State;
    State.Microphone.DeviceName = "@DEFAULT_SOURCE@";
    State.Microphone.Description = "Microphone";

    if (!initializeOutput(&State)) {
        cleanupRecorder(&State);
        pthread_mutex_destroy(&State.MuxMutex);
        return 1;
    }

    GlobalState = &State;
    struct sigaction SignalAction = {0};
    SignalAction.sa_handler = requestStop;
    sigemptyset(&SignalAction.sa_mask);
    sigaction(SIGINT, &SignalAction, NULL);
    sigaction(SIGTERM, &SignalAction, NULL);

    State.Video.State = &State;
    if (pthread_create(&State.Video.Thread, NULL, videoThreadMain, &State) != 0) {
        fprintf(stderr, "Could not start video thread.\n");
        requestFatal(&State);
    } else {
        State.Video.Started = true;
    }

    if (!atomic_load_explicit(&State.FatalError, memory_order_relaxed)) {
        if (pthread_create(&State.DesktopAudio.Thread, NULL, audioThreadMain, &State.DesktopAudio) != 0) {
            fprintf(stderr, "Could not start desktop audio thread.\n");
            requestFatal(&State);
        } else {
            State.DesktopAudio.Started = true;
        }
    }

    if (!atomic_load_explicit(&State.FatalError, memory_order_relaxed)) {
        if (pthread_create(&State.Microphone.Thread, NULL, audioThreadMain, &State.Microphone) != 0) {
            fprintf(stderr, "Could not start microphone thread.\n");
            requestFatal(&State);
        } else {
            State.Microphone.Started = true;
        }
    }

    if (State.Video.Started) {
        pthread_join(State.Video.Thread, NULL);
    }
    if (State.DesktopAudio.Started) {
        pthread_join(State.DesktopAudio.Thread, NULL);
    }
    if (State.Microphone.Started) {
        pthread_join(State.Microphone.Thread, NULL);
    }

    int Result = av_write_trailer(State.Output);
    if (Result < 0) {
        printError("av_write_trailer", Result);
        requestFatal(&State);
    }

    bool RecordingSuccess = !atomic_load_explicit(&State.FatalError, memory_order_relaxed);

    cleanupRecorder(&State);
    pthread_mutex_destroy(&State.MuxMutex);
    GlobalState = NULL;

    if (RecordingSuccess) {
        printf("Recording saved: %s\n", State.OutputPath);
    }

    return RecordingSuccess ? 0 : 1;
}
