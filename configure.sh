#!/usr/bin/env bash
set -euo pipefail

ProjectRoot="$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)"
MainSource="$ProjectRoot/main.c"
BuildRoot="$ProjectRoot/.build"
DownloadRoot="$BuildRoot/download"
SourceRoot="$BuildRoot/source"
Prefix="$BuildRoot/prefix"
ConfigFile="$BuildRoot/config.sh"
Jobs="${JOBS:-$(getconf _NPROCESSORS_ONLN 2>/dev/null || printf '1')}"
Compiler="${CC:-cc}"

DistroId="unknown"
DistroName="Unknown Linux"
PackageManager="unknown"

if [[ -r /etc/os-release ]]; then
    # shellcheck disable=SC1091
    . /etc/os-release
    DistroId="${ID:-unknown}"
    DistroName="${NAME:-Unknown Linux}${VERSION_ID:+ ${VERSION_ID}}"
fi

if command -v emerge >/dev/null 2>&1; then
    PackageManager=emerge
elif command -v pacman >/dev/null 2>&1; then
    PackageManager=pacman
elif command -v apt-get >/dev/null 2>&1; then
    PackageManager=apt
elif command -v dnf >/dev/null 2>&1; then
    PackageManager=dnf
elif command -v zypper >/dev/null 2>&1; then
    PackageManager=zypper
elif command -v apk >/dev/null 2>&1; then
    PackageManager=apk
elif command -v xbps-install >/dev/null 2>&1; then
    PackageManager=xbps
fi

FfmpegVersion="9.0.2"
PulseVersion="14.2"
LibvaVersion="2.24.1"
LibX11Version="1.8.13"
LibdrmVersion="main"
XcbVersion="1.17.0"
XcbProtoVersion="1.17.0"
XtransVersion="1.6.0"

ColorBlue=''
ColorGreen=''
ColorYellow=''
ColorRed=''
ColorReset=''
if [[ -t 1 ]]; then
    ColorBlue=$'\033[36m'
    ColorGreen=$'\033[32m'
    ColorYellow=$'\033[33m'
    ColorRed=$'\033[31m'
    ColorReset=$'\033[0m'
fi

SourceDeps=()
SystemDeps=()
BuiltDeps=()
BuildInputs=()

fail()
{
    printf '%b[ERROR]%b %s\n' "$ColorRed" "$ColorReset" "$1" >&2
    exit 1
}

info()
{
    printf '%b==>%b %s\n' "$ColorBlue" "$ColorReset" "$1"
}

ok()
{
    printf '%b[OK]%b %s\n' "$ColorGreen" "$ColorReset" "$1"
}

need()
{
    command -v "$1" >/dev/null 2>&1 || BuildInputs+=("$2")
}

packageHint()
{
    local Topic="$1"
    case "$PackageManager" in
        emerge)
            case "$Topic" in
                build) printf 'emerge sys-devel/gcc sys-devel/make net-misc/curl app-arch/tar dev-build/meson dev-util/ninja dev-util/pkgconf' ;;
                runtime) printf 'emerge x11-base/xorg-server x11-libs/libX11 x11-libs/libXext x11-libs/libXrandr media-libs/libva media-libs/mesa media-sound/pulseaudio' ;;
                dynamic) printf 'emerge sys-devel/gcc x11-libs/libX11 x11-libs/libXrandr media-video/ffmpeg' ;;
                static) printf 'emerge --ask sys-libs/glibc sys-devel/gcc' ;;
            esac
            ;;
        pacman)
            case "$Topic" in
                build) printf 'sudo pacman -S --needed base-devel curl tar meson ninja pkgconf' ;;
                runtime) printf 'sudo pacman -S --needed xorg-server libx11 libxext libxrandr libva mesa pipewire-pulse' ;;
                dynamic) printf 'sudo pacman -S --needed base-devel libx11 libxrandr ffmpeg' ;;
                static) printf 'sudo pacman -S --needed glibc gcc' ;;
            esac
            ;;
        apt)
            case "$Topic" in
                build) printf 'sudo apt-get install build-essential curl tar meson ninja-build pkg-config' ;;
                runtime) printf 'sudo apt-get install xorg libx11-6 libxext6 libxrandr2 libva2 mesa-va-drivers pipewire-pulse' ;;
                dynamic) printf 'sudo apt-get install build-essential pkg-config libx11-dev libxrandr-dev libavdevice-dev libavformat-dev libavcodec-dev libavutil-dev libswresample-dev libswscale-dev' ;;
                static) printf 'sudo apt-get install libc6-dev gcc' ;;
            esac
            ;;
        dnf)
            case "$Topic" in
                build) printf 'sudo dnf install gcc make curl tar meson ninja-build pkgconf-pkg-config' ;;
                runtime) printf 'sudo dnf install xorg-x11-server-Xorg libX11 libXext libXrandr libva mesa-va-drivers pipewire-pulseaudio' ;;
                dynamic) printf 'sudo dnf install gcc pkgconf-pkg-config libX11-devel libXrandr-devel ffmpeg-free-devel' ;;
                static) printf 'sudo dnf install glibc-static libgcc-static' ;;
            esac
            ;;
        zypper)
            case "$Topic" in
                build) printf 'sudo zypper install gcc make curl tar meson ninja pkg-config' ;;
                runtime) printf 'sudo zypper install xorg-x11-server libX11-6 libXext6 libXrandr2 libva2 Mesa-libva pipewire-pulseaudio' ;;
                dynamic) printf 'sudo zypper install gcc pkg-config libX11-devel libXrandr-devel ffmpeg-devel' ;;
                static) printf 'sudo zypper install glibc-devel gcc' ;;
            esac
            ;;
        apk)
            case "$Topic" in
                build) printf 'sudo apk add gcc musl-dev make curl tar meson ninja pkgconf' ;;
                runtime) printf 'sudo apk add xorg-server libx11 libxext libxrandr libva mesa-va-gallium pulseaudio' ;;
                dynamic) printf 'sudo apk add build-base pkgconf libx11-dev libxrandr-dev ffmpeg-dev' ;;
                static) printf 'sudo apk add musl-dev gcc' ;;
            esac
            ;;
        xbps)
            case "$Topic" in
                build) printf 'sudo xbps-install -S gcc make curl tar meson ninja pkg-config' ;;
                runtime) printf 'sudo xbps-install -S xorg-server libX11 libXext libXrandr libva mesa-dri pipewire-pulse' ;;
                dynamic) printf 'sudo xbps-install -S gcc pkg-config libX11-devel libXrandr-devel ffmpeg-devel' ;;
                static) printf 'sudo xbps-install -S glibc-devel gcc' ;;
            esac
            ;;
        *)
            case "$Topic" in
                build) printf 'Install a C compiler, make, curl, tar, pkg-config, and Meson/Ninja when a missing dependency needs to be built.' ;;
                runtime) printf 'Install an X11 server, X11 client libraries, Mesa/VA-API, and a PulseAudio-compatible audio server.' ;;
                dynamic) printf 'Install the X11/XRandR and FFmpeg development packages required by main.c.' ;;
                static) printf 'Install the static C runtime libraries provided by your distribution.' ;;
            esac
            ;;
    esac
}

usage()
{
    cat <<USAGE
Usage: ./configure.sh [--static|--dynamic] [--clean-run]

Inspect main.c, verify dependencies, and prepare .build/config.sh.
Static mode may download/build missing static dependencies under .build/.
Dynamic mode only uses system development libraries and never downloads dependencies.
The recorder is never started.

Options:
  --static       Prepare a fully static build (default).
  --dynamic      Prepare a dynamically linked build using system libraries.
  --clean-run    Remove generated build state while preserving downloaded source archives.
  -h, --help     Show this help.
USAGE
}

BuildMode=static
CleanRun=0
while [[ $# -gt 0 ]]; do
    case "$1" in
        --static)
            BuildMode=static
            ;;
        --dynamic)
            BuildMode=dynamic
            ;;
        --clean-run)
            CleanRun=1
            ;;
        -h|--help)
            usage
            exit 0
            ;;
        *)
            usage >&2
            exit 2
            ;;
    esac
    shift
done

[[ -f "$MainSource" ]] || fail "main.c was not found."

# build.sh is a generated artifact. Never leave a stale build script behind
# when starting a new configuration run. It is recreated only after success.
rm -f -- "$ProjectRoot/build.sh"

info "Detecting build tools"
need "$Compiler" "A C23 compiler (gcc or clang)."
need pkg-config "pkg-config for dependency metadata and link flags."

if [[ "$BuildMode" == static ]]; then
    need make "GNU make or compatible make."
    need curl "curl for downloading missing dependencies."
    need tar "tar for extracting dependency sources."
    need nm "binutils nm for validating static FFmpeg archives."
    need autoreconf "Autotools (autoconf, automake, libtool) for generating missing configure scripts."
fi

if [[ "${#BuildInputs[@]}" -gt 0 ]]; then
    printf '%b[BUILD TOOLS MISSING]%b\n' "$ColorRed" "$ColorReset"
    printf 'Distribution: %s\n' "$DistroName"
    printf 'Package manager: %s\n' "$PackageManager"
    printf '  - %s\n' "${BuildInputs[@]}"
    printf '\nSuggested installation:\n  %s\n' "$(packageHint build)"
    exit 1
fi

CompilerPath="$(command -v "$Compiler")"

if [[ "$BuildMode" == dynamic ]]; then
    # Dynamic mode uses only system-wide libraries. No private .build tree is
    # created, read, or used in this mode.
    export PKG_CONFIG_PATH=
    export PKG_CONFIG_LIBDIR=

    info "Checking system dynamic dependencies"
    DynamicMissing=0
    DynamicCppflags=''
    DynamicCflags='-O2 -Wall -Wextra -Wpedantic -ffunction-sections -fdata-sections'
    DynamicLdFlags=''
    DynamicLibs=''
    DynamicProbe="$ProjectRoot/.screenRecorder-dynamic-probe.c"
    DynamicBinary="$ProjectRoot/.screenRecorder-dynamic-probe"

    checkDynamicDependency()
    {
        local Name="$1"
        local Header="$2"
        local PackageNames="$3"
        local FallbackLibs="$4"
        local ProbeSource=""
        local ProbeCflags=""
        local ProbeLibs=""

        if command -v pkg-config >/dev/null 2>&1 && [[ -n "$PackageNames" ]] && pkg-config --exists $PackageNames 2>/dev/null; then
            ProbeCflags="$(pkg-config --cflags $PackageNames 2>/dev/null || true)"
            ProbeLibs="$(pkg-config --libs $PackageNames 2>/dev/null || true)"
        else
            ProbeCflags=''
            ProbeLibs="$FallbackLibs"
        fi

        ProbeSource="#include <$Header>\nint main(void) { return 0; }\n"
        printf '%b' "$ProbeSource" > "$DynamicProbe"

        if "$CompilerPath" -std=c23 -fsyntax-only $ProbeCflags "$DynamicProbe" >/dev/null 2>&1 &&
           "$CompilerPath" -std=c23 "$DynamicProbe" $ProbeCflags $ProbeLibs -pthread -lm -ldl -o "$DynamicBinary" >/dev/null 2>&1; then
            DynamicCppflags+=" $ProbeCflags"
            DynamicLdFlags+=" $(pkg-config --libs-only-L $PackageNames 2>/dev/null || true)"
            DynamicLibs+=" $ProbeLibs"
            ok "$Name: system headers and libraries are usable"
        else
            printf '%b[MISSING]%b %s: system development files could not compile and link the required interface
' "$ColorRed" "$ColorReset" "$Name"
            DynamicMissing=1
        fi
    }

    checkDynamicDependency \
        "X11/XShm/XRandR" \
        "X11/Xlib.h" \
        "x11 xext xrandr" \
        "-lXrandr -lXext -lX11"

    checkDynamicDependency \
        "PulseAudio" \
        "pulse/simple.h" \
        "libpulse-simple" \
        "-lpulse-simple -lpulse"

    checkDynamicDependency \
        "FFmpeg" \
        "libavcodec/avcodec.h" \
        "libavformat libavcodec libavutil" \
        "-lavformat -lavcodec -lavutil"

    rm -f -- "$DynamicProbe" "$DynamicBinary"

    if [[ "$DynamicMissing" -ne 0 ]]; then
        printf '\nSuggested installation for %s:\n  %s\n' "$DistroName" "$(packageHint dynamic)" >&2
        printf '\nDynamic configuration failed. Install the missing development dependencies and run ./configure.sh --dynamic again.\n' >&2
        exit 1
    fi

    info "Checking unavoidable runtime facilities"
    RuntimeOk=1
    if [[ -n "${DISPLAY:-}" ]]; then
        ok "X11 DISPLAY is available: $DISPLAY"
    else
        printf '%b[MISSING]%b No DISPLAY is set; the recorder needs an X11 session at runtime.\n' "$ColorRed" "$ColorReset"
        RuntimeOk=0
    fi

    RenderNodeFound=0
    shopt -s nullglob
    RenderNodes=(/dev/dri/renderD*)
    shopt -u nullglob
    for Node in "${RenderNodes[@]}"; do
        if [[ -r "$Node" && -w "$Node" ]]; then
            ok "Accessible DRM render node: $Node"
            RenderNodeFound=1
            break
        fi
    done
    if [[ "$RenderNodeFound" -eq 0 ]]; then
        printf '%b[WARNING]%b No writable /dev/dri/renderD* device is available; hardware VA-API encoders may be unavailable, but software fallback encoders can still be used.\n' "$ColorYellow" "$ColorReset"
    fi

    PulseSocket="${XDG_RUNTIME_DIR:-/run/user/$UID}/pulse/native"
    if [[ -S "$PulseSocket" ]] || (command -v pactl >/dev/null 2>&1 && pactl info >/dev/null 2>&1); then
        ok "PulseAudio-compatible audio server is reachable"
    else
        printf '%b[MISSING]%b No reachable PulseAudio-compatible audio server was detected.\n' "$ColorRed" "$ColorReset"
        RuntimeOk=0
    fi

    if [[ "$RuntimeOk" -ne 1 ]]; then
        printf '\nRuntime requirements are missing.\n' >&2
        printf 'Suggested installation:\n  %s\n' "$(packageHint runtime)" >&2
        exit 1
    fi

    info "Generating dynamic build configuration"
    BuildScript="$ProjectRoot/build.sh"
    cat > "$BuildScript" <<BUILD_SCRIPT
#!/usr/bin/env bash
set -euo pipefail

ProjectRoot="\$(CDPATH= cd -- "\$(dirname -- "\$0")" && pwd)"
Output="\$ProjectRoot/screenRecorder"
MainSource="\$ProjectRoot/main.c"

[[ -f "\$MainSource" ]] || {
    printf 'build.sh: main.c not found: %s\n' "\$MainSource" >&2
    exit 1
}

printf '==> Building screenRecorder\n'
printf '    Compiler: %s\n' '$CompilerPath'
printf '    Source:   %s\n' "\$MainSource"
printf '    Output:   %s\n' "\$Output"
printf '    Mode:     dynamic\n'

Compiler='$CompilerPath'
CppFlags='$DynamicCppflags'
CFlags='$DynamicCflags'
LdFlags='$DynamicLdFlags'
Libs='$DynamicLibs -pthread -lm -ldl -latomic'

# shellcheck disable=SC2086
"\$Compiler" \\
    -std=c23 \\
    \$CppFlags \\
    \$CFlags \\
    "\$MainSource" \\
    \$LdFlags \\
    -Wl,--gc-sections \\
    \$Libs \\
    -o "\$Output"

printf '==> Built %s\n' "\$Output"
BUILD_SCRIPT
    chmod 755 "$BuildScript"

    printf '\n%bDynamic configuration complete.%b\n' "$ColorGreen" "$ColorReset"
    printf 'All direct development dependencies and required runtime interfaces are present.\n'
    printf '\nBuild the program with:\n  ./build.sh\n'
    exit 0
fi

mkdir -p "$DownloadRoot" "$SourceRoot" "$Prefix"

if [[ "$CleanRun" -eq 1 ]]; then
    info "Removing generated static build state for a clean configuration run"
    # Keep downloaded source archives so a clean static configuration can reuse
    # them without downloading again.
    rm -rf -- "$SourceRoot" "$Prefix" "$ConfigFile" "$BuildRoot/meson-libdrm" "$BuildRoot/meson-libva"
    mkdir -p "$DownloadRoot" "$SourceRoot" "$Prefix"
fi

sourceDependency()
{
    local Header="$1"
    case "$Header" in
        X11/Xlib.h|X11/Xatom.h|X11/cursorfont.h|X11/keysym.h)
            addDependency libX11
            ;;
        X11/extensions/Xrandr.h)
            addDependency libXrandr
            ;;
        X11/extensions/XShm.h)
            addDependency libXext
            ;;        pulse/error.h|pulse/simple.h)
            addDependency pulseaudio
            ;;
        libavcodec/avcodec.h|libavformat/avformat.h|libavutil/channel_layout.h|libavutil/error.h|libavutil/frame.h|libavutil/hwcontext.h|libavutil/hwcontext_vaapi.h|libavutil/imgutils.h|libavutil/mathematics.h|libavutil/opt.h|libavutil/samplefmt.h)
            addDependency ffmpeg
            ;;
    esac
}

addDependency()
{
    local Dependency="$1"
    local Existing
    for Existing in "${SourceDeps[@]}"; do
        [[ "$Existing" == "$Dependency" ]] && return
    done
    SourceDeps+=("$Dependency")
}

info "Reading dependency requirements from main.c"
while IFS= read -r Header; do
    sourceDependency "$Header"
done < <(sed -nE 's/^#[[:space:]]*include[[:space:]]*<([^>]+)>.*$/\1/p' "$MainSource")

# Transitive static dependencies required by the libraries used by main.c.
if printf '%s\n' "${SourceDeps[@]}" | grep -qx 'libX11' || printf '%s\n' "${SourceDeps[@]}" | grep -qx 'libXrandr'; then
    addDependency libXau
    addDependency libXdmcp
fi
if printf '%s\n' "${SourceDeps[@]}" | grep -qx 'libX11'; then
    addDependency xcbproto
    addDependency libxcb
    addDependency xtrans
fi
if printf '%s\n' "${SourceDeps[@]}" | grep -qx 'libXrandr'; then
    addDependency libXrender
    addDependency libXext
    addDependency libX11
fi
if printf '%s\n' "${SourceDeps[@]}" | grep -qx 'libXext'; then
    addDependency libX11
fi
if printf '%s\n' "${SourceDeps[@]}" | grep -qx 'ffmpeg'; then
    addDependency libva
    addDependency libdrm
fi

printf 'Dependencies inferred from main.c:\n'
printf '  %s\n' "${SourceDeps[@]}"

systemIncludeDirs()
{
    local Module="$1"
    local Dir
    if command -v pkg-config >/dev/null 2>&1 && pkg-config --exists "$Module" 2>/dev/null; then
        pkg-config --cflags-only-I "$Module" 2>/dev/null | tr ' ' '\n' | sed -n 's/^-I//p'
    fi
    printf '%s\n' /usr/include /usr/local/include /usr/include/* 2>/dev/null | sed '/[*]/d'
}

systemLibDirs()
{
    local Module="$1"
    if command -v pkg-config >/dev/null 2>&1 && pkg-config --exists "$Module" 2>/dev/null; then
        pkg-config --variable=libdir "$Module" 2>/dev/null || true
    fi
    printf '%s\n' /usr/lib /usr/lib64 /usr/local/lib /usr/local/lib64 /lib /lib64
}

hasHeader()
{
    local Header="$1"
    local Module="$2"
    local Dir
    while IFS= read -r Dir; do
        [[ -z "$Dir" ]] && continue
        [[ -f "$Dir/$Header" ]] && return 0
    done < <(systemIncludeDirs "$Module")
    return 1
}

hasArchive()
{
    local Archive="$1"
    local Module="$2"
    local Dir
    while IFS= read -r Dir; do
        [[ -z "$Dir" ]] && continue
        [[ -f "$Dir/$Archive" ]] && return 0
    done < <(systemLibDirs "$Module")
    return 1
}

hasPulseCommonArchive()
{
    local Dir
    while IFS= read -r Dir; do
        [[ -z "$Dir" ]] && continue
        compgen -G "$Dir/libpulsecommon-*.a" >/dev/null 2>&1 && return 0
    done < <(systemLibDirs libpulse)
    return 1
}

depSpec()
{
    case "$1" in
        libXau)      printf '%s\n' 'X11/Xauth.h|libXau.a|xau|1.0.12|https://www.x.org/releases/individual/lib/libXau-1.0.12.tar.xz|autotools' ;;
        libXdmcp)    printf '%s\n' 'X11/Xdmcp.h|libXdmcp.a|xdmcp|1.1.5|https://www.x.org/releases/individual/lib/libXdmcp-1.1.5.tar.xz|autotools' ;;
        libX11)      printf '%s\n' "X11/Xlib.h|libX11.a|x11|$LibX11Version|https://www.x.org/releases/individual/lib/libX11-$LibX11Version.tar.xz|x11" ;;
        xcbproto)    printf '%s\n' 'xproto.xml|xcb-proto.pc|xcb-proto|1.17.0|https://xorg.freedesktop.org/archive/individual/proto/xcb-proto-1.17.0.tar.xz|proto' ;;
        libxcb)      printf '%s\n' 'xcb/xcb.h|libxcb.a|xcb|1.17.0|https://xcb.freedesktop.org/dist/libxcb-1.17.0.tar.xz|xcb' ;;
        xtrans)      printf '%s\n' 'X11/Xtrans/Xtranssock.c|xtrans.marker|xtrans|1.6.0|https://xorg.freedesktop.org/archive/individual/lib/xtrans-1.6.0.tar.xz|xtrans' ;;
        libXext)     printf '%s\n' 'X11/extensions/XShm.h|libXext.a|xext|1.3.6|https://www.x.org/releases/individual/lib/libXext-1.3.6.tar.xz|autotools' ;;
        libXrender)  printf '%s\n' 'X11/extensions/Xrender.h|libXrender.a|xrender|0.9.12|https://www.x.org/releases/individual/lib/libXrender-0.9.12.tar.xz|autotools' ;;
        libXrandr)   printf '%s\n' 'X11/extensions/Xrandr.h|libXrandr.a|xrandr|1.5.4|https://www.x.org/releases/individual/lib/libXrandr-1.5.4.tar.xz|autotools' ;;
        libdrm)      printf '%s\n' 'drm/drm.h|libdrm.a|libdrm|main|https://gitlab.freedesktop.org/mesa/libdrm/-/archive/main/libdrm-main.tar.gz|meson' ;;
        libva)       printf '%s\n' 'va/va.h|libva.a|libva|2.24.1|https://github.com/intel/libva/archive/refs/tags/2.24.1.tar.gz|meson' ;;
        pulseaudio)  printf '%s\n' 'pulse/simple.h|libpulse.a|libpulse|14.2|https://freedesktop.org/software/pulseaudio/releases/pulseaudio-14.2.tar.xz|autotools' ;;
        ffmpeg)      printf '%s\n' 'libavcodec/avcodec.h|libavcodec.a|ffmpeg|9.0.2|https://ffmpeg.org/releases/ffmpeg-9.0.2.tar.xz|ffmpeg' ;;
        *) return 1 ;;
    esac
}


findPrefixArchive()
{
    local Archive="$1"
    find "$Prefix" -type f -name "$Archive" -print -quit 2>/dev/null || true
}

findPrefixPulseCommonArchive()
{
    find "$Prefix" -type f -name 'libpulsecommon-*.a' -print -quit 2>/dev/null || true
}

findPrefixPulseSimpleArchive()
{
    find "$Prefix" -type f -name 'libpulse-simple.a' -print -quit 2>/dev/null || true
}

ffmpegArchiveHasSymbol()
{
    local Symbol="$1"
    local Archive
    Archive="$(findPrefixArchive libavcodec.a)"
    [[ -n "$Archive" ]] || return 1

    if command -v nm >/dev/null 2>&1; then
        nm -g "$Archive" 2>/dev/null | grep -Eq "[[:space:]]${Symbol}$"
        return $?
    fi

    return 1
}

ffmpegLocalReady()
{
    local Directory="$SourceRoot/ffmpeg-$FfmpegVersion"
    local Archive

    [[ -f "$Directory/config_components.h" ]] || return 1
    grep -q '^#define CONFIG_H264_VAAPI_ENCODER 1$' "$Directory/config_components.h" || return 1
    grep -q '^#define CONFIG_MPEG4_ENCODER 1$' "$Directory/config_components.h" || return 1

    Archive="$(findPrefixArchive libavcodec.a)"
    [[ -n "$Archive" ]] || return 1
    ffmpegArchiveHasSymbol ff_mpeg4_encoder || return 1

    return 0
}

localDepReady()
{
    local Dependency="$1"
    local Header Archive Module Version Url Kind
    IFS='|' read -r Header Archive Module Version Url Kind <<< "$(depSpec "$Dependency")"

    case "$Dependency" in
        ffmpeg)
            ffmpegLocalReady
            ;;
        libva)
            [[ -f "$Prefix/include/$Header" ]] && [[ -n "$(findPrefixArchive libva.a)" ]] && [[ -n "$(findPrefixArchive libva-drm.a)" ]]
            ;;
        pulseaudio)
            [[ -f "$Prefix/include/$Header" ]] && [[ -n "$(findPrefixArchive "$Archive")" ]] && [[ -n "$(findPrefixPulseSimpleArchive)" ]] && [[ -n "$(findPrefixPulseCommonArchive)" ]]
            ;;
        xcbproto)
            [[ -f "$Prefix/share/pkgconfig/xcb-proto.pc" ]] && [[ -f "$Prefix/share/xcb/$Header" || -f "$Prefix/$Header" ]]
            ;;
        xtrans)
            [[ -f "$Prefix/share/X11/Xtrans/$Header" || -f "$Prefix/include/$Header" ]]
            ;;
        *)
            [[ -f "$Prefix/include/$Header" ]] && [[ -n "$(findPrefixArchive "$Archive")" ]]
            ;;
    esac
}

downloadedArchive()
{
    local Name="$1"
    local Version="$2"
    local Url="$3"
    local Archive="$DownloadRoot/$(basename "$Url")"
    [[ -f "$Archive" ]] || return 1
    tar -tf "$Archive" >/dev/null 2>&1
}

needsBuild()
{
    local Dependency="$1"
    local Header Archive Module Version Url Kind
    IFS='|' read -r Header Archive Module Version Url Kind <<< "$(depSpec "$Dependency")"

    # 1. Reuse a complete private-prefix build.
    if localDepReady "$Dependency"; then
        return 1
    fi

    # 2. Reuse a complete system static dependency when available.
    case "$Dependency" in
        pulseaudio)
            if hasHeader "$Header" "$Module" && hasArchive "$Archive" "$Module" && \
               hasArchive libpulse-simple.a libpulse-simple && hasPulseCommonArchive; then
                return 1
            fi
            ;;
        libva)
            if hasHeader "$Header" "$Module" && hasArchive libva.a "$Module" && hasArchive libva-drm.a libva-drm; then
                return 1
            fi
            ;;
        *)
            if hasHeader "$Header" "$Module" && hasArchive "$Archive" "$Module"; then
                return 1
            fi
            ;;
    esac

    # 3. A valid source archive already exists locally; build it instead of downloading.
    if downloadedArchive "$Dependency" "$Version" "$Url"; then
        printf '       %s: cached source archive found; no download required.\n' "$Dependency" >&2
    else
        printf '       %s: no cached source archive found; it will be downloaded only if a build is required.\n' "$Dependency" >&2
    fi

    # 4. Dependency is not usable; caller must build it. getArchive() will reuse
    #    an existing archive and only download when it is genuinely absent.
    return 0
}

getArchive()
{
    local Name="$1"
    local Url="$2"
    local Archive="$DownloadRoot/$(basename "$Url")"
    local TemporaryArchive="$Archive.part"

    if [[ -f "$Archive" ]] && tar -tf "$Archive" >/dev/null 2>&1; then
        printf '       %s source archive already present; reusing it.\n' "$Name" >&2
        printf '%s\n' "$Archive"
        return 0
    fi

    rm -f -- "$Archive" "$TemporaryArchive"
    info "Downloading $Name" >&2

    # Network connections can fail after TLS negotiation or during transfer.
    # Keep incomplete files out of the final archive path and retry transient
    # failures before giving up. HTTP/1.1 avoids a number of problematic HTTP/2
    # proxy/server combinations seen in minimal environments.
    if ! curl \
        --fail \
        --location \
        --show-error \
        --silent \
        --http1.1 \
        --retry 10 \
        --retry-all-errors \
        --retry-delay 3 \
        --connect-timeout 20 \
        --max-time 900 \
        -o "$TemporaryArchive" \
        "$Url"; then
        rm -f -- "$TemporaryArchive"
        fail "Could not download $Name from $Url"
    fi

    if ! tar -tf "$TemporaryArchive" >/dev/null 2>&1; then
        rm -f -- "$TemporaryArchive"
        fail "Downloaded archive for $Name is invalid: $Url"
    fi

    mv -- "$TemporaryArchive" "$Archive"
    printf '%s\n' "$Archive"
}

extractArchive()
{
    local Name="$1"
    local Version="$2"
    local Url="$3"
    local Archive
    local SourceDirectory

    Archive="$(getArchive "$Name-$Version" "$Url")"

    # Do not assume the archive directory is Name-Version. Several upstream
    # projects use a different source directory name (for example xcb-proto).
    local TopLevel
    TopLevel="$(tar -tf "$Archive" | awk -F/ 'NF {print $1; exit}')"
    [[ -n "$TopLevel" ]] || fail "Could not determine top-level directory from $Archive"

    SourceDirectory="$SourceRoot/$TopLevel"
    [[ -d "$SourceDirectory" ]] && { printf '%s\n' "$SourceDirectory"; return; }

    tar -xf "$Archive" -C "$SourceRoot"
    [[ -d "$SourceDirectory" ]] || fail "Archive $Archive did not extract expected source directory $TopLevel"
    printf '%s\n' "$SourceDirectory"
}

setBuildEnvironment()
{
    export PKG_CONFIG_PATH="$Prefix/lib/pkgconfig:$Prefix/share/pkgconfig:${PKG_CONFIG_PATH:-}"
    export CPPFLAGS="-I$Prefix/include ${CPPFLAGS:-}"
    export CFLAGS="-O2 -fPIC ${CFLAGS:-}"
    export LDFLAGS="-L$Prefix/lib ${LDFLAGS:-}"
}

buildAutotools()
{
    local Name="$1"
    local Directory="$2"
    shift 2

    local Marker="$Directory/.screenRecorderBuilt"
    local Header Archive Module Version Url Kind
    IFS='|' read -r Header Archive Module Version Url Kind <<< "$(depSpec "$Name")"

    if [[ -f "$Marker" ]]; then
        case "$Name" in
            pulseaudio)
                if [[ -n "$(findPrefixArchive libpulse.a)" ]] && [[ -n "$(findPrefixPulseSimpleArchive)" ]] && [[ -n "$(findPrefixPulseCommonArchive)" ]]; then
                    printf '       %s already built.\n' "$Name"
                    return
                fi
                ;;
            libva)
                if [[ -n "$(findPrefixArchive libva.a)" ]] && [[ -n "$(findPrefixArchive libva-drm.a)" ]]; then
                    printf '       %s already built.\n' "$Name"
                    return
                fi
                ;;
            *)
                if [[ -n "$(findPrefixArchive "$Archive")" ]]; then
                    printf '       %s already built.\n' "$Name"
                    return
                fi
                ;;
        esac
        rm -f "$Marker"
    fi

    info "Compiling static $Name"
    (
        cd "$Directory"
        ./configure \
            --prefix="$Prefix" \
            --libdir="$Prefix/lib" \
            --enable-static \
            --disable-shared \
            --disable-silent-rules \
            "$@"
        make -j"$Jobs"
        make install
        touch "$Marker"
    )
}

buildMeson()
{
    local Name="$1"
    local Directory="$2"
    shift 2
    local BuildDirectory="$BuildRoot/meson-$Name"

    command -v meson >/dev/null 2>&1 || fail "meson is required to build $Name; install it and rerun configure.sh."
    command -v ninja >/dev/null 2>&1 || fail "ninja is required to build $Name; install it and rerun configure.sh."

    if [[ -f "$BuildDirectory/.screenRecorderBuilt" ]]; then
        case "$Name" in
            pulseaudio)
                if [[ -n "$(findPrefixArchive libpulse.a)" ]] && [[ -n "$(findPrefixPulseSimpleArchive)" ]] && [[ -n "$(findPrefixPulseCommonArchive)" ]]; then
                    printf '       %s already built.\n' "$Name"
                    return
                fi
                ;;
            libdrm)
                if [[ -n "$(findPrefixArchive libdrm.a)" ]]; then
                    printf '       %s already built.\n' "$Name"
                    return
                fi
                ;;
            *)
                :
                ;;
        esac
        rm -f "$BuildDirectory/.screenRecorderBuilt"
    fi

    info "Compiling static $Name"
    meson setup "$BuildDirectory" "$Directory" \
        --prefix="$Prefix" \
        --libdir=lib \
        --buildtype=release \
        --default-library=static \
        "$@"
    meson compile -C "$BuildDirectory" -j "$Jobs"
    meson install -C "$BuildDirectory"
    touch "$BuildDirectory/.screenRecorderBuilt"
}

buildX11Component()
{
    local Dependency="$1"
    local Header Archive Module Version Url Kind
    IFS='|' read -r Header Archive Module Version Url Kind <<< "$(depSpec "$Dependency")"
    local Directory
    Directory="$(extractArchive "$Dependency" "$Version" "$Url")"

    case "$Dependency" in
        libX11)
            (
                export CPPFLAGS="$CPPFLAGS -D_GNU_SOURCE -DHAVE_SYS_IOCTL_H=1 -include sys/ioctl.h"
                export CFLAGS="$CFLAGS -D_GNU_SOURCE -pthread"
                export LDFLAGS="$LDFLAGS -pthread"
                export ACLOCAL="aclocal -I $Prefix/share/aclocal"
                buildAutotools "$Dependency" "$Directory" --enable-xthreads
            )
            ;;
        libxcb)
            buildAutotools "$Dependency" "$Directory"
            ;;
        xcbproto)
            buildAutotools "$Dependency" "$Directory"
            ;;
        xtrans)
            mkdir -p "$Prefix/include/X11/Xtrans"
            cp -a "$Directory/"* "$Prefix/include/X11/Xtrans/"
            touch "$Prefix/include/X11/Xtrans/xtrans.marker"
            ;;
        libXau|libXdmcp|libXext|libXrender|libXrandr)
            buildAutotools "$Dependency" "$Directory"
            ;;
    esac
}

buildLibdrm()
{
    local Directory
    Directory="$(extractArchive libdrm main https://gitlab.freedesktop.org/mesa/libdrm/-/archive/main/libdrm-main.tar.gz)"
    buildMeson libdrm "$Directory" -Dtests=false
}

findVaDriverPath()
{
    local Dir Candidate

    while IFS= read -r Dir; do
        [[ -n "$Dir" ]] || continue
        if compgen -G "$Dir/*_drv_video.so" >/dev/null 2>&1; then
            printf '%s\n' "$Dir"
            return
        fi
    done < <(find /usr/lib /usr/lib64 /usr/local/lib /usr/local/lib64 /lib /lib64 -type d -name dri -print 2>/dev/null)

    if command -v pkg-config >/dev/null 2>&1; then
        local PcLibDir
        for PcLibDir in /usr/lib64/pkgconfig /usr/lib/pkgconfig /usr/local/lib64/pkgconfig /usr/local/lib/pkgconfig; do
            [[ -d "$PcLibDir" ]] || continue
            Candidate="$(PKG_CONFIG_LIBDIR="$PcLibDir" PKG_CONFIG_PATH='' pkg-config --variable=driverdir libva 2>/dev/null || true)"
            if [[ -n "$Candidate" && -d "$Candidate" ]]; then
                printf '%s\n' "$Candidate"
                return
            fi
        done
    fi
}

buildLibva()
{
    local Directory
    Directory="$(extractArchive libva "$LibvaVersion" "https://github.com/intel/libva/archive/refs/tags/$LibvaVersion.tar.gz")"

    (
        cd "$Directory"
        sed -i '/^AC_DISABLE_STATIC[[:space:]]*$/d' configure.ac
        if [[ ! -x ./configure ]]; then
            NOCONFIGURE=1 ./autogen.sh
        fi
        rm -f config.status config.cache
    )

    local DriverPath
    DriverPath="$(findVaDriverPath)"
    [[ -n "$DriverPath" ]] || fail "Could not find an installed VA-API driver directory under /usr/lib* or /lib*."

    buildAutotools libva "$Directory" \
        --enable-drm \
        --disable-x11 \
        --disable-wayland \
        --disable-glx \
        --disable-docs \
        --with-drivers-path="$DriverPath"

    [[ -n "$(findPrefixArchive libva.a)" ]] || fail "libva build completed but libva.a was not installed under $Prefix."
    [[ -n "$(findPrefixArchive libva-drm.a)" ]] || fail "libva build completed but libva-drm.a was not installed under $Prefix."
}

buildPulse()
{
    local Directory
    Directory="$(extractArchive pulseaudio "$PulseVersion" "https://freedesktop.org/software/pulseaudio/releases/pulseaudio-$PulseVersion.tar.xz")"
    local Marker="$Directory/.screenRecorderBuilt"

    if [[ -f "$Marker" ]] && [[ -n "$(findPrefixArchive libpulse.a)" ]] && [[ -n "$(findPrefixPulseSimpleArchive)" ]] && [[ -n "$(findPrefixPulseCommonArchive)" ]]; then
        printf '       pulseaudio already built.\n'
        return
    fi

    info "Compiling static PulseAudio client libraries $PulseVersion"
    (
        cd "$Directory"
        ./configure \
            --prefix="$Prefix" \
            --libdir="$Prefix/lib" \
            --disable-maintainer-mode \
            --disable-shared \
            --enable-static \
            --disable-tests \
            --disable-manpages \
            --disable-x11 \
            --disable-glib2 \
            --disable-gtk3 \
            --disable-gsettings \
            --disable-gconf \
            --disable-avahi \
            --disable-jack \
            --disable-asyncns \
            --disable-tcpwrap \
            --disable-lirc \
            --disable-dbus \
            --disable-bluez5 \
            --disable-udev \
            --disable-hal-compat \
            --disable-openssl \
            --disable-systemd-daemon \
            --disable-systemd-login \
            --disable-systemd-journal \
            --disable-oss-output \
            --disable-oss-wrapper \
            --disable-esound \
            --disable-solaris \
            --disable-waveout \
            --disable-alsa \
            --without-fftw \
            --without-speex \
            --without-soxr \
            --with-database=simple \
            --without-caps

        make -C src libpulse.la libpulse-simple.la -j"$Jobs"
        make -C src install-libLTLIBRARIES install-pulseincludeHEADERS
        install -m 644 libpulse.pc libpulse-simple.pc "$Prefix/lib/pkgconfig/"
    )

    [[ -n "$(findPrefixArchive libpulse.a)" ]] || fail "PulseAudio build completed without libpulse.a."
    [[ -n "$(findPrefixPulseSimpleArchive)" ]] || fail "PulseAudio build completed without libpulse-simple.a."
    [[ -n "$(findPrefixPulseCommonArchive)" ]] || fail "PulseAudio build completed without libpulsecommon static archive."
    touch "$Marker"
}

buildFfmpeg()
{
    local Directory
    Directory="$(extractArchive ffmpeg "$FfmpegVersion" "https://ffmpeg.org/releases/ffmpeg-$FfmpegVersion.tar.xz")"
    local Marker="$Directory/.screenRecorderBuilt"

    if ffmpegLocalReady; then
        printf '       FFmpeg already built with required encoders.\n'
        return
    fi

    rm -f "$Marker"

    info "Compiling minimal static FFmpeg $FfmpegVersion"
    (
        cd "$Directory"
        ./configure \
            --prefix="$Prefix" \
            --libdir="$Prefix/lib" \
            --disable-shared \
            --enable-static \
            --disable-programs \
            --disable-doc \
            --disable-network \
            --disable-autodetect \
            --disable-avdevice \
            --disable-avfilter \
            --disable-x86asm \
            --disable-zlib \
            --enable-protocol=file \
            --enable-muxer=mp4 \
            --enable-encoder=aac \
            --enable-encoder=mpeg4 \
            --enable-encoder=h264_vaapi \
            --enable-vaapi \
            --disable-xlib \
            --enable-libdrm \
            --pkg-config-flags=--static \
            --extra-cflags="-I$Prefix/include" \
            --extra-ldflags="-L$Prefix/lib" \
            --disable-debug

        if ! grep -q '^#define CONFIG_MPEG4_ENCODER 1$' config_components.h; then
            fail "FFmpeg configure did not enable the native mpeg4 encoder."
        fi

        make -j"$Jobs"
        make install
        touch "$Marker"
    )
}

setBuildEnvironment

for Dependency in "${SourceDeps[@]}"; do
    case "$Dependency" in
        libXau|libXdmcp|xcbproto|libxcb|xtrans|libX11|libXext|libXrender|libXrandr|libdrm|libva|pulseaudio|ffmpeg)
            ;;
        *) fail "No build recipe exists for dependency '$Dependency' inferred from main.c." ;;
    esac
done

BuildOrder=(libXau libXdmcp xcbproto libxcb xtrans libX11 libXext libXrender libXrandr libdrm libva pulseaudio ffmpeg)
for Dependency in "${BuildOrder[@]}"; do
    Required=0
    for Existing in "${SourceDeps[@]}"; do
        [[ "$Existing" == "$Dependency" ]] && Required=1 && break
    done
    [[ "$Required" -eq 1 ]] || continue

    if ! needsBuild "$Dependency"; then
        SystemDeps+=("$Dependency")
        ok "$Dependency: usable headers and static archive found on the system"
        continue
    fi

    case "$Dependency" in
        libXau|libXdmcp|xcbproto|libxcb|xtrans|libX11|libXext|libXrender|libXrandr) buildX11Component "$Dependency" ;;
        libdrm) buildLibdrm ;;
        libva) buildLibva ;;
        pulseaudio) buildPulse ;;
        ffmpeg) buildFfmpeg ;;
    esac

    BuiltDeps+=("$Dependency")
    ok "$Dependency: static build installed under $Prefix"
    setBuildEnvironment
done

# Check libc/static toolchain archives required for a fully static ELF.
info "Checking static C runtime support"
StaticRuntimeMissing=0
for Archive in libc.a libm.a libpthread.a libdl.a; do
    ArchivePath="$($Compiler -print-file-name="$Archive" 2>/dev/null || true)"
    if [[ -z "$ArchivePath" || "$ArchivePath" == "$Archive" || ! -f "$ArchivePath" ]]; then
        printf '%b[MISSING]%b compiler static runtime archive: %s\n' "$ColorRed" "$ColorReset" "$Archive"
        StaticRuntimeMissing=1
    else
        ok "$Archive"
    fi
done
if [[ "$StaticRuntimeMissing" -ne 0 ]]; then
    printf '\nSuggested installation:\n  %s\n' "$(packageHint static)" >&2
    fail "The compiler does not provide all static libc archives required for a single static binary."
fi

info "Generating build configuration"

# Resolve the exact dependency set once. Prefer dependencies prepared in the
# private prefix, but allow system pkg-config metadata when configure.sh verified
# that the corresponding static archives are already usable on the system.
export PKG_CONFIG_PATH="$Prefix/lib/pkgconfig:$Prefix/share/pkgconfig:/usr/lib/pkgconfig:/usr/lib64/pkgconfig:/usr/local/lib/pkgconfig:/usr/local/share/pkgconfig:/usr/local/lib64/pkgconfig"
unset PKG_CONFIG_LIBDIR

ConfigCflags='-O2 -Wall -Wextra -Wpedantic -ffunction-sections -fdata-sections'
ConfigCppflags="-I$Prefix/include"
ConfigLdfags="-L$Prefix/lib"
ConfigLibs=""

appendPkgConfig()
{
    local PackageNames="$1"

    if ! pkg-config --exists $PackageNames 2>/dev/null; then
        return 1
    fi

    ConfigCppflags+=" $(pkg-config --cflags-only-I $PackageNames)"
    ConfigLdfags+=" $(pkg-config --libs-only-L $PackageNames)"
    ConfigLibs+=" $(pkg-config --static --libs $PackageNames)"
    return 0
}

findStaticArchive()
{
    local Dependency="$1"
    local Header Archive Module Version Url Kind
    IFS='|' read -r Header Archive Module Version Url Kind <<< "$(depSpec "$Dependency")"
    local Dir Candidate

    Candidate="$(findPrefixArchive "$Archive")"
    if [[ -n "$Candidate" ]]; then
        printf '%s\n' "$Candidate"
        return 0
    fi

    while IFS= read -r Dir; do
        [[ -z "$Dir" ]] && continue
        Candidate="$Dir/$Archive"
        if [[ -f "$Candidate" ]]; then
            printf '%s\n' "$Candidate"
            return 0
        fi
    done < <(systemLibDirs "$Module")

    return 1
}

addStaticArchiveDirectory()
{
    local Dependency="$1"
    local ArchivePath
    ArchivePath="$(findStaticArchive "$Dependency")" || return 1
    ConfigLdfags+=" -L$(dirname "$ArchivePath")"

    if [[ "$Dependency" == pulseaudio ]]; then
        local PulseSimplePath PulseCommonPath
        PulseSimplePath="$(findPrefixPulseSimpleArchive)"
        [[ -n "$PulseSimplePath" ]] || return 1
        ConfigLdfags+=" -L$(dirname "$PulseSimplePath")"
        PulseCommonPath="$(findPrefixPulseCommonArchive)"
        if [[ -z "$PulseCommonPath" ]]; then
            PulseCommonPath="$(find /usr/lib /usr/lib64 /usr/local/lib /usr/local/lib64 /lib /lib64 -maxdepth 3 -type f -name 'libpulsecommon-*.a' -print -quit 2>/dev/null || true)"
        fi
        [[ -n "$PulseCommonPath" ]] || return 1
        ConfigLdfags+=" -L$(dirname "$PulseCommonPath")"
    fi

    if [[ "$Dependency" == libva ]]; then
        local VaDrmArchive LocalVaDrmArchive
        LocalVaDrmArchive="$(findPrefixArchive libva-drm.a)"
        if [[ -n "$LocalVaDrmArchive" ]]; then
            ConfigLdfags+=" -L$(dirname "$LocalVaDrmArchive")"
        else
            VaDrmArchive="$(find /usr/lib /usr/lib64 /usr/local/lib /usr/local/lib64 /lib /lib64 -maxdepth 2 -type f -name 'libva-drm.a' -print -quit 2>/dev/null || true)"
            [[ -n "$VaDrmArchive" ]] || return 1
            ConfigLdfags+=" -L$(dirname "$VaDrmArchive")"
        fi
    fi
}

# X11: prefer the private static dependency tree configured above.
# XRandR requires XRender at link time, even though main.c does not call
# XRender directly. Keep the complete transitive static X11 link set.
if pkg-config --exists xrandr xext xrender x11 2>/dev/null; then
    ConfigCppflags+=" $(pkg-config --cflags-only-I xrandr xext xrender x11)"
    ConfigLdfags+=" $(pkg-config --libs-only-L xrandr xext xrender x11)"
    X11StaticLibs="$(pkg-config --static --libs xrandr xext xrender x11 | tr -s ' ')"
    ConfigLibs+=" $X11StaticLibs"
else
    fail "The configured static X11 dependency set is incomplete. Rerun ./configure.sh --clean-run."
fi

# PulseAudio client + its private static dependencies.
appendPkgConfig 'libpulse-simple' || {
    fail "The configured static PulseAudio dependency set is incomplete. Rerun ./configure.sh --clean-run."
}

# VA-API + libdrm.
appendPkgConfig 'libva libva-drm libdrm' || {
    fail "The configured static VA-API dependency set is incomplete. Rerun ./configure.sh --clean-run."
}

# Minimal FFmpeg built by configure.sh.
appendPkgConfig 'libavformat libavcodec libavutil' || {
    fail "The configured static FFmpeg dependency set is incomplete. Rerun ./configure.sh --clean-run."
}

# Ensure every static archive's actual directory is present in the final link.
# This is necessary when the system's .pc files omit non-standard directories
# such as PulseAudio's libpulsecommon directory.
for Dependency in libXau libXdmcp libxcb libX11 libXext libXrandr libdrm libva pulseaudio ffmpeg; do
    Required=0
    for Existing in "${SourceDeps[@]}"; do
        [[ "$Existing" == "$Dependency" ]] && Required=1 && break
    done
    [[ "$Required" -eq 1 ]] || continue
    addStaticArchiveDirectory "$Dependency" || fail "Static archive for $Dependency is missing after configuration."
done

# The generated configuration contains exact private-prefix search paths and
# only the static dependency graph prepared above.
ConfigLibs+=" -pthread -lm -ldl -latomic"

cat > "$ConfigFile" <<CONFIG
#!/usr/bin/env bash
# Generated by configure.sh. Do not edit.
ScreenRecorderProjectRoot='$ProjectRoot'
ScreenRecorderCompiler='$CompilerPath'
ScreenRecorderBuildMode='static'
ScreenRecorderCppFlags='${ConfigCppflags}'
ScreenRecorderCFlags='${ConfigCflags}'
ScreenRecorderLdFlags='${ConfigLdfags}'
ScreenRecorderLibs='${ConfigLibs}'
CONFIG
chmod 600 "$ConfigFile"

ok "Static dependency configuration prepared: $ConfigFile"

info "Checking unavoidable runtime facilities"
RuntimeOk=1
if [[ -n "${DISPLAY:-}" ]]; then
    ok "X11 DISPLAY is available: $DISPLAY"
else
    printf '%b[MISSING]%b No DISPLAY is set; the recorder needs an X11 session at runtime.\n' "$ColorRed" "$ColorReset"
    RuntimeOk=0
fi

RenderNodeFound=0
shopt -s nullglob
RenderNodes=(/dev/dri/renderD*)
shopt -u nullglob
for Node in "${RenderNodes[@]}"; do
    if [[ -r "$Node" && -w "$Node" ]]; then
        ok "Accessible DRM render node: $Node"
        RenderNodeFound=1
        break
    fi
done
if [[ "$RenderNodeFound" -eq 0 ]]; then
    printf '%b[MISSING]%b No writable /dev/dri/renderD* device is available to the current user.\n' "$ColorRed" "$ColorReset"
    RuntimeOk=0
fi

PulseSocket="${XDG_RUNTIME_DIR:-/run/user/$UID}/pulse/native"
if [[ -S "$PulseSocket" ]] || (command -v pactl >/dev/null 2>&1 && pactl info >/dev/null 2>&1); then
    ok "PulseAudio-compatible audio server is reachable"
else
    printf '%b[MISSING]%b No reachable PulseAudio-compatible audio server was detected.\n' "$ColorRed" "$ColorReset"
    RuntimeOk=0
fi

if [[ "$RuntimeOk" -eq 1 ]]; then
BuildScript="$ProjectRoot/build.sh"
cat > "$BuildScript" <<'BUILD_SCRIPT'
#!/usr/bin/env bash
set -euo pipefail

ProjectRoot="$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)"
ConfigFile="$ProjectRoot/.build/config.sh"
Output="$ProjectRoot/screenRecorder"

if [[ ! -f "$ConfigFile" ]]; then
    printf 'build.sh: .build/config.sh not found. Run ./configure.sh first.\n' >&2
    exit 1
fi

# shellcheck disable=SC1090
source "$ConfigFile"

: "${ScreenRecorderProjectRoot:?Missing ScreenRecorderProjectRoot in configure output}"
: "${ScreenRecorderCompiler:?Missing ScreenRecorderCompiler in configure output}"
: "${ScreenRecorderBuildMode:?Missing ScreenRecorderBuildMode in configure output}"
: "${ScreenRecorderCppFlags:?Missing ScreenRecorderCppFlags in configure output}"
: "${ScreenRecorderCFlags:?Missing ScreenRecorderCFlags in configure output}"
: "${ScreenRecorderLdFlags:?Missing ScreenRecorderLdFlags in configure output}"
: "${ScreenRecorderLibs:?Missing ScreenRecorderLibs in configure output}"

MainSource="$ScreenRecorderProjectRoot/main.c"
[[ -f "$MainSource" ]] || {
    printf 'build.sh: main.c not found: %s\n' "$MainSource" >&2
    exit 1
}

printf '==> Building screenRecorder\n'
printf '    Compiler: %s\n' "$ScreenRecorderCompiler"
printf '    Source:   %s\n' "$MainSource"
printf '    Output:   %s\n' "$Output"
printf '    Mode:     %s\n' "$ScreenRecorderBuildMode"

StaticFlags=()
if [[ "$ScreenRecorderBuildMode" == static ]]; then
    StaticFlags=(-static)
fi

# shellcheck disable=SC2086
"$ScreenRecorderCompiler" \
    -std=c23 \
    $ScreenRecorderCppFlags \
    $ScreenRecorderCFlags \
    "$MainSource" \
    $ScreenRecorderLdFlags \
    "${StaticFlags[@]}" \
    -Wl,--gc-sections \
    -Wl,--start-group \
    $ScreenRecorderLibs \
    -Wl,--end-group \
    -o "$Output"

printf '==> Built %s\n' "$Output"
BUILD_SCRIPT
chmod 755 "$BuildScript"

    printf '\n%bConfiguration complete.%b\n' "$ColorGreen" "$ColorReset"
    printf 'All build requirements and unavoidable runtime interfaces are present.\n'
    printf '\nBuild the program with:\n  ./build.sh\n'
else
    printf '\n%bRuntime requirements are missing.%b\n' "$ColorYellow" "$ColorReset"
    printf 'Suggested system installation:\n  %s\n' "$(packageHint runtime)"
    printf '\nAfter installing/configuring the missing components, run ./configure.sh again.\n'
    exit 1
fi
