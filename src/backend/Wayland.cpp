#include <algorithm>
#include <aquamarine/backend/Wayland.hpp>
#include <wayland.hpp>
#include <xdg-shell.hpp>
#include "Shared.hpp"
#include "FormatUtils.hpp"
#include <cstring>
#include <cstdio>
#include <cstdlib>
#include <xf86drm.h>
#include <gbm.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <unistd.h>

using namespace Aquamarine;
using namespace Hyprutils::Memory;
using namespace Hyprutils::Math;
#define SP CSharedPointer

static std::pair<int, std::string> openExclusiveShm() {
    // Only absolute paths can be shared across different shm_open() calls
    srand(time(nullptr));
    std::string name = std::format("/aq{:x}", rand() % RAND_MAX);

    for (size_t i = 0; i < 69; ++i) {
        int fd = shm_open(name.c_str(), O_RDWR | O_CREAT | O_EXCL, 0600);
        if (fd >= 0)
            return {fd, name};
    }

    return {-1, ""};
}

static int allocateSHMFile(size_t len) {
    auto [fd, name] = openExclusiveShm();
    if (fd < 0)
        return -1;

    shm_unlink(name.c_str());

    int ret;
    do {
        ret = ftruncate(fd, len);
    } while (ret < 0 && errno == EINTR);

    if (ret < 0) {
        close(fd);
        return -1;
    }

    return fd;
}

wl_shm_format shmFormatFromDRM(uint32_t drmFormat) {
    switch (drmFormat) {
        case DRM_FORMAT_XRGB8888: return WL_SHM_FORMAT_XRGB8888;
        case DRM_FORMAT_ARGB8888: return WL_SHM_FORMAT_ARGB8888;
        default: return (wl_shm_format)drmFormat;
    }

    return (wl_shm_format)drmFormat;
}

Aquamarine::CWaylandBackend::~CWaylandBackend() {
    outputs.clear();
    keyboards.clear();
    pointers.clear();
    idleCallbacks.clear();

    waylandState.dmabufFeedback.reset();
    waylandState.dmabuf.reset();
    waylandState.shm.reset();
    waylandState.compositor.reset();
    waylandState.xdg.reset();
    waylandState.seat.reset();
    waylandState.registry.reset();

    if (waylandState.display) {
        wl_display_disconnect(waylandState.display);
        waylandState.display = nullptr;
    }

    if (drmState.fd >= 0) {
        close(drmState.fd);
        drmState.fd = -1;
    }
}

eBackendType Aquamarine::CWaylandBackend::type() {
    return AQ_BACKEND_WAYLAND;
}

Aquamarine::CWaylandBackend::CWaylandBackend(SP<CBackend> backend_) : backend(backend_) {
    ;
}

bool Aquamarine::CWaylandBackend::start() {
    backend->log(AQ_LOG_DEBUG, "Starting the Wayland backend!");

    waylandState.display = wl_display_connect(nullptr);

    if (!waylandState.display) {
        backend->log(AQ_LOG_ERROR, "Wayland backend cannot start: wl_display_connect failed (is a wayland compositor running?)");
        return false;
    }

    auto XDGCURRENTDESKTOP = getenv("XDG_CURRENT_DESKTOP");
    backend->log(AQ_LOG_DEBUG, std::format("Connected to a wayland compositor: {}", (XDGCURRENTDESKTOP ? XDGCURRENTDESKTOP : "unknown (XDG_CURRENT_DEKSTOP unset?)")));

    waylandState.registry = makeShared<CCWlRegistry>((wl_proxy*)wl_display_get_registry(waylandState.display));

    backend->log(AQ_LOG_DEBUG, std::format("Got registry at 0x{:x}", (uintptr_t)waylandState.registry->resource()));

    waylandState.registry->setGlobal([this](CCWlRegistry* r, uint32_t id, const char* name, uint32_t version) {
        TRACE(backend->log(AQ_LOG_TRACE, std::format(" | received global: {} (version {}) with id {}", name, version, id)));

        const std::string NAME = name;

        if (NAME == "wl_seat") {
            const uint32_t BIND = std::min(version, 9u);
            TRACE(backend->log(AQ_LOG_TRACE, std::format("  > binding to global: {} (version {}) with id {}", name, BIND, id)));
            waylandState.seat = makeShared<CCWlSeat>((wl_proxy*)wl_registry_bind((wl_registry*)waylandState.registry->resource(), id, &wl_seat_interface, BIND));
            initSeat();
        } else if (NAME == "xdg_wm_base") {
            const uint32_t BIND = std::min(version, 6u);
            TRACE(backend->log(AQ_LOG_TRACE, std::format("  > binding to global: {} (version {}) with id {}", name, BIND, id)));
            waylandState.xdg = makeShared<CCXdgWmBase>((wl_proxy*)wl_registry_bind((wl_registry*)waylandState.registry->resource(), id, &xdg_wm_base_interface, BIND));
            initShell();
        } else if (NAME == "wl_compositor") {
            const uint32_t BIND = std::min(version, 6u);
            TRACE(backend->log(AQ_LOG_TRACE, std::format("  > binding to global: {} (version {}) with id {}", name, BIND, id)));
            waylandState.compositor =
                makeShared<CCWlCompositor>((wl_proxy*)wl_registry_bind((wl_registry*)waylandState.registry->resource(), id, &wl_compositor_interface, BIND));
        } else if (NAME == "wl_shm") {
            const uint32_t BIND = std::min(version, 1u);
            TRACE(backend->log(AQ_LOG_TRACE, std::format("  > binding to global: {} (version {}) with id {}", name, BIND, id)));
            waylandState.shm = makeShared<CCWlShm>((wl_proxy*)wl_registry_bind((wl_registry*)waylandState.registry->resource(), id, &wl_shm_interface, BIND));
        } else if (NAME == "zwp_linux_dmabuf_v1") {
            const uint32_t BIND = std::min(version, 4u);
            TRACE(backend->log(AQ_LOG_TRACE, std::format("  > binding to global: {} (version {}) with id {}", name, BIND, id)));
            waylandState.dmabuf =
                makeShared<CCZwpLinuxDmabufV1>((wl_proxy*)wl_registry_bind((wl_registry*)waylandState.registry->resource(), id, &zwp_linux_dmabuf_v1_interface, BIND));
            if (!initDmabuf()) {
                backend->log(AQ_LOG_ERROR, "Wayland backend cannot start: zwp_linux_dmabuf_v1 init failed");
                waylandState.dmabufFailed = true;
            }
        }
    });
    waylandState.registry->setGlobalRemove([this](CCWlRegistry* r, uint32_t id) { backend->log(AQ_LOG_DEBUG, std::format("Global {} removed", id)); });

    wl_display_roundtrip(waylandState.display);

    if (!waylandState.xdg || !waylandState.compositor || !waylandState.seat || !waylandState.shm) {
        backend->log(AQ_LOG_ERROR, "Wayland backend cannot start: Missing protocols");
        return false;
    }

    // if the host compositor does not offer a working zwp_linux_dmabuf_v1 (e.g.
    // WSLg, which only speaks wl_shm), fall back to a host-memory (shm) present
    // path. Frames are rendered on the GPU and copied into wl_shm buffers.
    shmMode = !waylandState.dmabuf || waylandState.dmabufFailed;
    if (shmMode) {
        waylandState.dmabuf.reset();
        waylandState.dmabufFeedback.reset();
        waylandState.dmabufFailed = false;
        // wl_shm mandates these two formats; advertise them as our render formats.
        shmFormats = {
            SDRMFormat{.drmFormat = DRM_FORMAT_ARGB8888, .modifiers = {DRM_FORMAT_MOD_LINEAR}},
            SDRMFormat{.drmFormat = DRM_FORMAT_XRGB8888, .modifiers = {DRM_FORMAT_MOD_LINEAR}},
        };
        backend->log(AQ_LOG_DEBUG, "Wayland backend: no dmabuf available, using a host-memory (wl_shm) present path");
    }

    dispatchEvents();

    createOutput();

    return true;
}

int Aquamarine::CWaylandBackend::drmFD() {
    return drmState.fd;
}

int Aquamarine::CWaylandBackend::drmRenderNodeFD() {
    // creation already attempts to use the rendernode, so just return same fd as drmFD().
    return drmState.fd;
}

bool Aquamarine::CWaylandBackend::createOutput(const std::string& szName) {
    std::string name = szName;
    if (name.empty()) {
        // skip past any auto-name slots already claimed by an explicit createOutput,
        // so we never hand out a duplicate WAYLAND-N (see #185).
        do {
            name = std::format("WAYLAND-{}", ++lastOutputID);
        } while (std::ranges::any_of(outputs, [&name](const auto& o) { return o->name == name; }));
    } else if (std::ranges::any_of(outputs, [&name](const auto& o) { return o->name == name; })) {
        backend->log(AQ_LOG_ERROR, std::format("Wayland: refusing to create output {}, name already in use", name));
        return false;
    }

    auto o  = outputs.emplace_back(SP<CWaylandOutput>(new CWaylandOutput(name, self)));
    o->self = o;
    if (backend->ready)
        o->swapchain = CSwapchain::create(backend->primaryAllocator, self.lock());
    idleCallbacks.emplace_back([this, o]() { backend->events.newOutput.emit(SP<IOutput>(o)); });
    return true;
}

std::vector<Hyprutils::Memory::CSharedPointer<SPollFD>> Aquamarine::CWaylandBackend::pollFDs() {
    if (!waylandState.display)
        return {};

    return {makeShared<SPollFD>(wl_display_get_fd(waylandState.display), [this]() { dispatchEvents(); })};
}

bool Aquamarine::CWaylandBackend::dispatchEvents() {
    wl_display_flush(waylandState.display);

    if (wl_display_prepare_read(waylandState.display) == 0) {
        wl_display_read_events(waylandState.display);
        wl_display_dispatch_pending(waylandState.display);
    } else
        wl_display_dispatch(waylandState.display);

    int ret = 0;
    do {
        ret = wl_display_dispatch_pending(waylandState.display);
        wl_display_flush(waylandState.display);
    } while (ret > 0);

    // dispatch frames
    if (backend->ready) {
        for (auto const& f : idleCallbacks) {
            f();
        }
        idleCallbacks.clear();
    }

    // idle callbacks (e.g. the first frame after a configure) may have queued
    // requests like a buffer attach/commit or a frame callback. Flush them now,
    // otherwise the host never sees the commit and never sends frame.done, which
    // stalls the render loop (notably the very first frame).
    wl_display_flush(waylandState.display);

    return true;
}

uint32_t Aquamarine::CWaylandBackend::capabilities() {
    return AQ_BACKEND_CAPABILITY_POINTER;
}

bool Aquamarine::CWaylandBackend::setCursor(Hyprutils::Memory::CSharedPointer<IBuffer> buffer, const Hyprutils::Math::Vector2D& hotspot) {
    // TODO:
    return true;
}

void Aquamarine::CWaylandBackend::onReady() {
    for (auto const& o : outputs) {
        o->swapchain = CSwapchain::create(backend->primaryAllocator, self.lock());
        if (!o->swapchain) {
            backend->log(AQ_LOG_ERROR, std::format("Output {} failed: swapchain creation failed", o->name));
            continue;
        }
    }
}

Aquamarine::CWaylandKeyboard::CWaylandKeyboard(SP<CCWlKeyboard> keyboard_, Hyprutils::Memory::CWeakPointer<CWaylandBackend> backend_) : keyboard(keyboard_), backend(backend_) {
    if (!keyboard->resource())
        return;

    backend->backend->log(AQ_LOG_DEBUG, "New wayland keyboard wl_keyboard");

    keyboard->setKey([this](CCWlKeyboard* r, uint32_t serial, uint32_t timeMs, uint32_t key, wl_keyboard_key_state state) {
        events.key.emit(SKeyEvent{
            .timeMs  = timeMs,
            .key     = key,
            .pressed = state == WL_KEYBOARD_KEY_STATE_PRESSED,
        });
    });

    keyboard->setModifiers([this](CCWlKeyboard* r, uint32_t serial, uint32_t depressed, uint32_t latched, uint32_t locked, uint32_t group) {
        events.modifiers.emit(SModifiersEvent{
            .depressed = depressed,
            .latched   = latched,
            .locked    = locked,
            .group     = group,
        });
    });
}

Aquamarine::CWaylandKeyboard::~CWaylandKeyboard() {
    ;
}

const std::string& Aquamarine::CWaylandKeyboard::getName() {
    return name;
}

Aquamarine::CWaylandPointer::CWaylandPointer(SP<CCWlPointer> pointer_, Hyprutils::Memory::CWeakPointer<CWaylandBackend> backend_) : pointer(pointer_), backend(backend_) {
    if (!pointer->resource())
        return;

    backend->backend->log(AQ_LOG_DEBUG, "New wayland pointer wl_pointer");

    pointer->setMotion([this](CCWlPointer* r, uint32_t serial, wl_fixed_t x, wl_fixed_t y) {
        const auto STATE = backend->focusedOutput->state->state();

        if (!backend->focusedOutput || (!STATE.mode && !STATE.customMode))
            return;

        const Vector2D size = STATE.customMode ? STATE.customMode->pixelSize : STATE.mode->pixelSize;

        Vector2D       local = {wl_fixed_to_double(x), wl_fixed_to_double(y)};
        local                = local / size;

        timespec now;
        clock_gettime(CLOCK_MONOTONIC, &now);

        events.warp.emit(SWarpEvent{
            .timeMs   = (uint32_t)(now.tv_sec * 1000 + now.tv_nsec / 1000000),
            .absolute = local,
        });
    });

    pointer->setEnter([this](CCWlPointer* r, uint32_t serial, wl_proxy* surface, wl_fixed_t x, wl_fixed_t y) {
        backend->lastEnterSerial = serial;

        for (auto const& o : backend->outputs) {
            if (o->waylandState.surface->resource() != surface)
                continue;

            backend->focusedOutput = o;
            backend->backend->log(AQ_LOG_DEBUG, std::format("[wayland] focus changed: {}", o->name));
            o->onEnter(pointer, serial);
            break;
        }
    });

    pointer->setLeave([this](CCWlPointer* r, uint32_t serial, wl_proxy* surface) {
        for (auto const& o : backend->outputs) {
            if (o->waylandState.surface->resource() != surface)
                continue;

            o->cursorState.serial = 0;
        }
    });

    pointer->setButton([this](CCWlPointer* r, uint32_t serial, uint32_t timeMs, uint32_t button, wl_pointer_button_state state) {
        events.button.emit(SButtonEvent{
            .timeMs  = timeMs,
            .button  = button,
            .pressed = state == WL_POINTER_BUTTON_STATE_PRESSED,
        });
    });

    pointer->setAxis([this](CCWlPointer* r, uint32_t timeMs, wl_pointer_axis axis, wl_fixed_t value) {
        events.axis.emit(SAxisEvent{
            .timeMs = timeMs,
            .axis   = axis == WL_POINTER_AXIS_HORIZONTAL_SCROLL ? AQ_POINTER_AXIS_HORIZONTAL : AQ_POINTER_AXIS_VERTICAL,
            .delta  = wl_fixed_to_double(value),
        });
    });

    pointer->setFrame([this](CCWlPointer* r) { events.frame.emit(); });
}

Aquamarine::CWaylandPointer::~CWaylandPointer() {
    ;
}

const std::string& Aquamarine::CWaylandPointer::getName() {
    return name;
}

void Aquamarine::CWaylandBackend::initSeat() {
    waylandState.seat->setCapabilities([this](CCWlSeat* r, wl_seat_capability cap) {
        const bool HAS_KEYBOARD = ((uint32_t)cap) & WL_SEAT_CAPABILITY_KEYBOARD;
        const bool HAS_POINTER  = ((uint32_t)cap) & WL_SEAT_CAPABILITY_POINTER;

        if (HAS_KEYBOARD && keyboards.empty()) {
            auto k = keyboards.emplace_back(makeShared<CWaylandKeyboard>(makeShared<CCWlKeyboard>(waylandState.seat->sendGetKeyboard()), self));
            idleCallbacks.emplace_back([this, k]() { backend->events.newKeyboard.emit(SP<IKeyboard>(k)); });
        } else if (!HAS_KEYBOARD && !keyboards.empty())
            keyboards.clear();

        if (HAS_POINTER && pointers.empty()) {
            auto p = pointers.emplace_back(makeShared<CWaylandPointer>(makeShared<CCWlPointer>(waylandState.seat->sendGetPointer()), self));
            idleCallbacks.emplace_back([this, p]() { backend->events.newPointer.emit(SP<IPointer>(p)); });
        } else if (!HAS_POINTER && !pointers.empty())
            pointers.clear();
    });
}

void Aquamarine::CWaylandBackend::initShell() {
    waylandState.xdg->setPing([](CCXdgWmBase* r, uint32_t serial) { r->sendPong(serial); });
}

bool Aquamarine::CWaylandBackend::initDmabuf() {
    waylandState.dmabufFeedback = makeShared<CCZwpLinuxDmabufFeedbackV1>(waylandState.dmabuf->sendGetDefaultFeedback());
    if (!waylandState.dmabufFeedback) {
        backend->log(AQ_LOG_ERROR, "initDmabuf: failed to get default feedback");
        return false;
    }

    waylandState.dmabufFeedback->setDone([this](CCZwpLinuxDmabufFeedbackV1* r) {
        // no-op
        backend->log(AQ_LOG_DEBUG, "zwp_linux_dmabuf_v1: Got done");
    });

    waylandState.dmabufFeedback->setMainDevice([this](CCZwpLinuxDmabufFeedbackV1* r, wl_array* deviceArr) {
        backend->log(AQ_LOG_DEBUG, "zwp_linux_dmabuf_v1: Got main device");

        dev_t device;
        ASSERT(deviceArr->size == sizeof(device));
        memcpy(&device, deviceArr->data, sizeof(device));

        drmDevice* drmDev;
        if (drmGetDeviceFromDevId(device, /* flags */ 0, &drmDev) != 0) {
            backend->log(AQ_LOG_ERROR, "zwp_linux_dmabuf_v1: drmGetDeviceFromDevId failed");
            return;
        }

        const char* name = nullptr;
        if (drmDev->available_nodes & (1 << DRM_NODE_RENDER))
            name = drmDev->nodes[DRM_NODE_RENDER];
        else {
            // Likely a split display/render setup. Pick the primary node and hope
            // Mesa will open the right render node under-the-hood.
            ASSERT(drmDev->available_nodes & (1 << DRM_NODE_PRIMARY));
            name = drmDev->nodes[DRM_NODE_PRIMARY];
            backend->log(AQ_LOG_WARNING, "zwp_linux_dmabuf_v1: DRM device has no render node, using primary.");
        }

        if (!name) {
            backend->log(AQ_LOG_ERROR, "zwp_linux_dmabuf_v1: no node name");
            drmFreeDevice(&drmDev);
            return;
        }

        drmState.nodeName = name;

        drmFreeDevice(&drmDev);

        backend->log(AQ_LOG_DEBUG, std::format("zwp_linux_dmabuf_v1: Got node {}", drmState.nodeName));
    });

    waylandState.dmabufFeedback->setFormatTable([this](CCZwpLinuxDmabufFeedbackV1* r, int32_t fd, uint32_t size) {
#pragma pack(push, 1)
        struct wlDrmFormatMarshalled {
            uint32_t drmFormat;
            char     pad[4];
            uint64_t modifier;
        };
#pragma pack(pop)
        static_assert(sizeof(wlDrmFormatMarshalled) == 16);

        auto formatTable = mmap(nullptr, size, PROT_READ, MAP_PRIVATE, fd, 0);
        if (formatTable == MAP_FAILED) {
            backend->log(AQ_LOG_ERROR, std::format("zwp_linux_dmabuf_v1: Failed to mmap the format table"));
            return;
        }

        const auto FORMATS = (wlDrmFormatMarshalled*)formatTable;

        for (size_t i = 0; i < size / 16; ++i) {
            auto& fmt = FORMATS[i];

            auto  modName = drmGetFormatModifierName(fmt.modifier);
            backend->log(AQ_LOG_DEBUG, std::format("zwp_linux_dmabuf_v1: Got format {} with modifier {}", fourccToName(fmt.drmFormat), modName ? modName : "UNKNOWN"));
            free(modName);

            auto it = std::ranges::find_if(dmabufFormats, [&fmt](const auto& e) { return e.drmFormat == fmt.drmFormat; });
            if (it == dmabufFormats.end()) {
                dmabufFormats.emplace_back(SDRMFormat{.drmFormat = fmt.drmFormat, .modifiers = {fmt.modifier}});
                continue;
            }

            it->modifiers.emplace_back(fmt.modifier);
        }

        munmap(formatTable, size);
    });

    wl_display_roundtrip(waylandState.display);

    if (!drmState.nodeName.empty()) {
        drmState.fd = open(drmState.nodeName.c_str(), O_RDWR | O_NONBLOCK | O_CLOEXEC);
        if (drmState.fd < 0) {
            backend->log(AQ_LOG_ERROR, std::format("zwp_linux_dmabuf_v1: Failed to open node {}", drmState.nodeName));
            return false;
        }

        backend->log(AQ_LOG_DEBUG, std::format("zwp_linux_dmabuf_v1: opened node {} with fd {}", drmState.nodeName, drmState.fd));
    }

    return true;
}

std::vector<SDRMFormat> Aquamarine::CWaylandBackend::getRenderFormats() {
    if (shmMode)
        return shmFormats;
    return dmabufFormats;
}

std::vector<SDRMFormat> Aquamarine::CWaylandBackend::getCursorFormats() {
    if (shmMode)
        return shmFormats;
    return dmabufFormats;
}

bool Aquamarine::CWaylandBackend::usesShmAllocator() {
    return shmMode;
}

SP<IAllocator> Aquamarine::CWaylandBackend::preferredAllocator() {
    return backend->primaryAllocator;
}

std::vector<SP<IAllocator>> Aquamarine::CWaylandBackend::getAllocators() {
    return {backend->primaryAllocator};
}

Hyprutils::Memory::CWeakPointer<IBackendImplementation> Aquamarine::CWaylandBackend::getPrimary() {
    return {};
}

Aquamarine::CWaylandOutput::CWaylandOutput(const std::string& name_, Hyprutils::Memory::CWeakPointer<CWaylandBackend> backend_) : backend(backend_) {
    name = name_;

    waylandState.surface = makeShared<CCWlSurface>(backend->waylandState.compositor->sendCreateSurface());

    if (!waylandState.surface->resource()) {
        backend->backend->log(AQ_LOG_ERROR, std::format("Output {} failed: no surface given. Errno: {}", name, errno));
        return;
    }

    waylandState.xdgSurface = makeShared<CCXdgSurface>(backend->waylandState.xdg->sendGetXdgSurface(waylandState.surface->resource()));

    if (!waylandState.xdgSurface->resource()) {
        backend->backend->log(AQ_LOG_ERROR, std::format("Output {} failed: no xdgSurface given. Errno: {}", name, errno));
        return;
    }

    waylandState.xdgSurface->setConfigure([this](CCXdgSurface* r, uint32_t serial) {
        backend->backend->log(AQ_LOG_DEBUG, std::format("Output {}: configure surface with {}", name, serial));
        r->sendAckConfigure(serial);

        // On the first configure the surface becomes presentable. If a buffer
        // was committed before we were configured, present it now.
        if (!xdgConfigured) {
            xdgConfigured = true;
            if (pendingCommitBuffer) {
                backend->backend->log(AQ_LOG_DEBUG, std::format("Output {}: flushing buffer deferred until first configure", name));
                waylandState.surface->sendAttach(pendingCommitBuffer->waylandState.buffer.get(), 0, 0);
                waylandState.surface->sendDamageBuffer(0, 0, INT32_MAX, INT32_MAX);
                readyForFrameCallback = true;
                pendingCommitBuffer.reset();

                // Bootstrap the render loop directly instead of going through
                // scheduleFrame()/idleCallbacks: idle callbacks are only drained
                // inside dispatchEvents(), and right after the first configure the
                // host has nothing to send us, so the wl fd stays quiet and the
                // idle callback would never run. By requesting a frame callback and
                // committing here, the host presents the buffer and sends
                // frame.done, which arrives over the wl fd, runs onFrameDone() from
                // inside dispatchEvents(), and from then on the loop self-sustains.
                frameScheduled             = false;
                frameScheduledWhileWaiting = false;
                waylandState.frameCallback = makeShared<CCWlCallback>(waylandState.surface->sendFrame());
                waylandState.frameCallback->setDone([this](CCWlCallback* r, uint32_t ms) { onFrameDone(); });
                waylandState.surface->sendCommit();
            }
        }
    });

    waylandState.xdgToplevel = makeShared<CCXdgToplevel>(waylandState.xdgSurface->sendGetToplevel());

    if (!waylandState.xdgToplevel->resource()) {
        backend->backend->log(AQ_LOG_ERROR, std::format("Output {} failed: no xdgToplevel given. Errno: {}", name, errno));
        return;
    }

    waylandState.xdgToplevel->setWmCapabilities(
        [this](CCXdgToplevel* r, wl_array* arr) { backend->backend->log(AQ_LOG_DEBUG, std::format("Output {}: wm_capabilities received", name)); });

    waylandState.xdgToplevel->setConfigure([this](CCXdgToplevel* r, int32_t w, int32_t h, wl_array* arr) {
        backend->backend->log(AQ_LOG_DEBUG, std::format("Output {}: configure toplevel with {}x{}", name, w, h));
        if (w == 0 || h == 0) {
            // The host (e.g. WSLg's Weston) left the size up to us. Default to
            // 1280x720, but allow AQ_WAYLAND_OUTPUT_SIZE=WIDTHxHEIGHT to request a
            // larger nested window (e.g. 1920x1080 for a fullscreen-ish desktop).
            w = 1280;
            h = 720;
            if (const char* sizeEnv = std::getenv("AQ_WAYLAND_OUTPUT_SIZE")) {
                int envW = 0, envH = 0;
                if (std::sscanf(sizeEnv, "%dx%d", &envW, &envH) == 2 && envW > 0 && envH > 0) {
                    w = envW;
                    h = envH;
                } else
                    backend->backend->log(AQ_LOG_ERROR,
                                          std::format("Output {}: ignoring malformed AQ_WAYLAND_OUTPUT_SIZE='{}', expected WIDTHxHEIGHT", name, sizeEnv));
            }
            backend->backend->log(AQ_LOG_DEBUG, std::format("Output {}: w/h is 0, using default {}x{}", name, w, h));
        }
        events.state.emit(SStateEvent{.size = {w, h}});
        // schedule the frame on the idle queue instead of emitting it synchronously:
        // the xdg_surface.configure that follows this toplevel.configure must be
        // acked (and flushed) before we may attach a buffer, otherwise the host
        // rejects the surface with "xdg_surface has never been configured".
        scheduleFrame(AQ_SCHEDULE_NEW_CONNECTOR);
    });

    waylandState.xdgToplevel->setClose([this](CCXdgToplevel* r) { destroy(); });

    waylandState.xdgToplevel->sendSetTitle(std::format("aquamarine - {}", name).c_str());
    waylandState.xdgToplevel->sendSetAppId("aquamarine");

    // Ask the host compositor (e.g. WSLg's Weston) to make this nested window
    // fullscreen, so the Hyprland session fills the screen like a real desktop
    // instead of floating in a normal window. Opt-in via AQ_WAYLAND_FULLSCREEN=1
    // (set by dev-env/start-wslg.sh when WSLG_FULLSCREEN=1). When honored, the
    // host replies with a configure carrying the real screen size, which the
    // handler above uses verbatim (so AQ_WAYLAND_OUTPUT_SIZE becomes a fallback).
    if (const char* fs = std::getenv("AQ_WAYLAND_FULLSCREEN"); fs && fs[0] == '1') {
        backend->backend->log(AQ_LOG_DEBUG, std::format("Output {}: requesting host fullscreen (AQ_WAYLAND_FULLSCREEN=1)", name));
        waylandState.xdgToplevel->sendSetFullscreen(nullptr);
    }

    auto inputRegion = makeShared<CCWlRegion>(backend->waylandState.compositor->sendCreateRegion());
    inputRegion->sendAdd(0, 0, INT32_MAX, INT32_MAX);

    waylandState.surface->sendSetInputRegion(inputRegion.get());
    waylandState.surface->sendAttach(nullptr, 0, 0);
    waylandState.surface->sendCommit();

    inputRegion->sendDestroy();

    backend->backend->log(AQ_LOG_DEBUG, std::format("Output {}: initialized", name));
}

Aquamarine::CWaylandOutput::~CWaylandOutput() {
    events.destroy.emit();
    if (waylandState.xdgToplevel)
        waylandState.xdgToplevel->sendDestroy();
    if (waylandState.xdgSurface)
        waylandState.xdgSurface->sendDestroy();
    if (waylandState.surface)
        waylandState.surface->sendDestroy();
}

std::vector<SDRMFormat> Aquamarine::CWaylandOutput::getRenderFormats() {
    // TODO
    // this is technically wrong because this returns the format table formats
    // the actually supported formats are given by tranche formats
    return backend->getRenderFormats();
}

bool Aquamarine::CWaylandOutput::pendingPageFlip() {
    return false;
}

bool Aquamarine::CWaylandOutput::destroy() {
    events.destroy.emit();
    waylandState.surface->sendAttach(nullptr, 0, 0);
    waylandState.surface->sendCommit();
    waylandState.frameCallback.reset();
    std::erase(backend->outputs, self.lock());
    return true;
}

bool Aquamarine::CWaylandOutput::test() {
    return true; // TODO:
}

bool Aquamarine::CWaylandOutput::commit() {
    Vector2D pixelSize = {};

    if (state->internalState.customMode)
        pixelSize = state->internalState.customMode->pixelSize;
    else if (state->internalState.mode)
        pixelSize = state->internalState.mode->pixelSize;
    else {
        backend->backend->log(AQ_LOG_ERROR, std::format("Output {}: pending state rejected: invalid mode", name));
        return false;
    }

    uint32_t format = state->internalState.drmFormat;

    if (format == DRM_FORMAT_INVALID) {
        backend->backend->log(AQ_LOG_ERROR, std::format("Output {}: pending state rejected: invalid format", name));
        return false;
    }

    if (!swapchain) {
        backend->backend->log(AQ_LOG_ERROR, std::format("Output {}: no swapchain, lying because it will soon be here", name));
        return true;
    }

    if (!swapchain->reconfigure(SSwapchainOptions{.length = swapchain->currentOptions().length, .size = pixelSize, .format = format})) {
        backend->backend->log(AQ_LOG_ERROR, std::format("Output {}: pending state rejected: swapchain failed reconfiguring", name));
        return false;
    }

    if (!state->internalState.buffer) {
        // if the consumer explicitly committed a null buffer, that's a violation.
        if (state->internalState.committed & COutputState::AQ_OUTPUT_STATE_BUFFER) {
            backend->backend->log(AQ_LOG_ERROR, std::format("Output {}: pending state rejected: no buffer", name));
            return false;
        }

        events.commit.emit();
        state->onCommit();
        return true;
    }

    auto wlBuffer = wlBufferFromBuffer(state->internalState.buffer);

    if (!wlBuffer) {
        backend->backend->log(AQ_LOG_ERROR, std::format("Output {}: pending state rejected: no wlBuffer??", name));
        return false;
    }

    if (wlBuffer->pendingRelease)
        backend->backend->log(AQ_LOG_WARNING, std::format("Output {}: pending state has a non-released buffer??", name));

    wlBuffer->pendingRelease = true;

    // We may not attach a buffer before the host has sent (and we have acked)
    // the first xdg_surface.configure. Stash the buffer and present it from the
    // configure handler instead.
    if (!xdgConfigured) {
        backend->backend->log(AQ_LOG_DEBUG, std::format("Output {}: deferring commit until first xdg_surface.configure", name));
        pendingCommitBuffer = wlBuffer;
        events.commit.emit();
        state->onCommit();
        needsFrame = false;
        return true;
    }

    waylandState.surface->sendAttach(wlBuffer->waylandState.buffer.get(), 0, 0);
    waylandState.surface->sendDamageBuffer(0, 0, INT32_MAX, INT32_MAX);
    waylandState.surface->sendCommit();

    // One-shot frame dump for verifying shm readback correctness (color/orientation).
    if (const char* dumpEnv = std::getenv("AQ_DUMP_FRAME")) {
        static int  frameNo  = 0;
        static bool dumped   = false;
        int         dumpAt   = atoi(dumpEnv);
        if (dumpAt < 1)
            dumpAt = 200;
        ++frameNo;
        auto shmAttrs = state->internalState.buffer->shm();
        if (!dumped && frameNo >= dumpAt && shmAttrs.success) {
            auto [data, fmt, len] = state->internalState.buffer->beginDataPtr(0);
            FILE* f               = fopen("/tmp/hypr-frame.ppm", "wb");
            if (f && data) {
                const int W = shmAttrs.size.x, H = shmAttrs.size.y, stride = shmAttrs.stride;
                fprintf(f, "P6\n%d %d\n255\n", W, H);
                for (int y = 0; y < H; ++y) {
                    const uint8_t* row = (const uint8_t*)data + (size_t)y * stride;
                    for (int x = 0; x < W; ++x) {
                        // XR24 (XRGB8888) little-endian in memory => bytes B,G,R,X
                        const uint8_t* px = row + x * 4;
                        fputc(px[2], f); // R
                        fputc(px[1], f); // G
                        fputc(px[0], f); // B
                    }
                }
                fclose(f);
                dumped = true;
                backend->backend->log(AQ_LOG_DEBUG, "AQ_DUMP_FRAME: wrote /tmp/hypr-frame.ppm");
            }
        }
    }

    readyForFrameCallback = true;

    events.commit.emit();
    state->onCommit();
    needsFrame = false;

    return true;
}

SP<IBackendImplementation> Aquamarine::CWaylandOutput::getBackend() {
    return SP<IBackendImplementation>(backend.lock());
}

SP<CWaylandBuffer> Aquamarine::CWaylandOutput::wlBufferFromBuffer(SP<IBuffer> buffer) {
    std::erase_if(backendState.buffers, [this](const auto& el) { return el.first.expired() || !swapchain->contains(el.first.lock()); });

    for (auto const& [k, v] : backendState.buffers) {
        if (k != buffer)
            continue;

        return v;
    }

    // create a new one
    auto wlBuffer = makeShared<CWaylandBuffer>(buffer, backend);

    if (!wlBuffer->good())
        return nullptr;

    backendState.buffers.emplace_back(std::make_pair<>(buffer, wlBuffer));

    return wlBuffer;
}

void Aquamarine::CWaylandOutput::sendFrameAndSetCallback() {
    events.frame.emit();
    frameScheduled = false;
    if (waylandState.frameCallback || !readyForFrameCallback)
        return;

    waylandState.frameCallback = makeShared<CCWlCallback>(waylandState.surface->sendFrame());
    waylandState.frameCallback->setDone([this](CCWlCallback* r, uint32_t ms) { onFrameDone(); });

    waylandState.surface->sendCommit();
}

void Aquamarine::CWaylandOutput::onFrameDone() {
    waylandState.frameCallback.reset();
    readyForFrameCallback = false;
    events.present.emit(IOutput::SPresentEvent{.presented = true});

    // FIXME: this is wrong, but otherwise we get bugs.
    // thanks @phonetic112
    scheduleFrame(AQ_SCHEDULE_NEEDS_FRAME);

    if (frameScheduledWhileWaiting)
        sendFrameAndSetCallback();
    else
        events.frame.emit();

    frameScheduledWhileWaiting = false;
}

bool Aquamarine::CWaylandOutput::setCursor(Hyprutils::Memory::CSharedPointer<IBuffer> buffer, const Hyprutils::Math::Vector2D& hotspot) {
    if (!cursorState.cursorSurface)
        cursorState.cursorSurface = makeShared<CCWlSurface>(backend->waylandState.compositor->sendCreateSurface());

    if (!cursorState.cursorSurface) {
        backend->backend->log(AQ_LOG_ERROR, std::format("Output {}: Failed to create a wl_surface for the cursor", name));
        return false;
    }

    if (!buffer) {
        cursorState.cursorBuffer.reset();
        cursorState.cursorWlBuffer.reset();
        if (!backend->pointers.empty())
            backend->pointers.at(0)->pointer->sendSetCursor(cursorState.serial, nullptr, cursorState.hotspot.x, cursorState.hotspot.y);
        return true;
    }

    cursorState.cursorBuffer = buffer;
    cursorState.hotspot      = hotspot;

    if (buffer->shm().success) {
        auto attrs                    = buffer->shm();
        auto [pixelData, fmt, bufLen] = buffer->beginDataPtr(0);

        int fd = allocateSHMFile(bufLen);
        if (fd < 0) {
            backend->backend->log(AQ_LOG_ERROR, std::format("Output {}: Failed to allocate a shm file", name));
            return false;
        }

        void* data = mmap(nullptr, bufLen, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
        if (data == MAP_FAILED) {
            backend->backend->log(AQ_LOG_ERROR, std::format("Output {}: Failed to mmap the cursor pixel data", name));
            close(fd);
            return false;
        }

        memcpy(data, pixelData, bufLen);
        munmap(data, bufLen);

        auto pool = makeShared<CCWlShmPool>(backend->waylandState.shm->sendCreatePool(fd, bufLen));
        if (!pool) {
            backend->backend->log(AQ_LOG_ERROR, std::format("Output {}: Failed to submit a wl_shm pool", name));
            close(fd);
            return false;
        }

        cursorState.cursorWlBuffer = makeShared<CCWlBuffer>(pool->sendCreateBuffer(0, attrs.size.x, attrs.size.y, attrs.stride, shmFormatFromDRM(attrs.format)));

        pool.reset();

        close(fd);
    } else if (auto attrs = buffer->dmabuf(); attrs.success) {
        auto params = makeShared<CCZwpLinuxBufferParamsV1>(backend->waylandState.dmabuf->sendCreateParams());

        for (int i = 0; i < attrs.planes; ++i) {
            params->sendAdd(attrs.fds.at(i), i, attrs.offsets.at(i), attrs.strides.at(i), attrs.modifier >> 32, attrs.modifier & 0xFFFFFFFF);
        }

        cursorState.cursorWlBuffer = makeShared<CCWlBuffer>(params->sendCreateImmed(attrs.size.x, attrs.size.y, attrs.format, (zwpLinuxBufferParamsV1Flags)0));
    } else {
        backend->backend->log(AQ_LOG_ERROR, std::format("Output {}: Failed to create a buffer for cursor: No known attrs (tried dmabuf / shm)", name));
        return false;
    }

    if (!cursorState.cursorWlBuffer) {
        backend->backend->log(AQ_LOG_ERROR, std::format("Output {}: Failed to create a buffer for cursor", name));
        return false;
    }

    cursorState.cursorSurface->sendSetBufferScale(1);
    cursorState.cursorSurface->sendSetBufferTransform(WL_OUTPUT_TRANSFORM_NORMAL);
    cursorState.cursorSurface->sendAttach(cursorState.cursorWlBuffer.get(), 0, 0);
    cursorState.cursorSurface->sendDamage(0, 0, INT32_MAX, INT32_MAX);
    cursorState.cursorSurface->sendCommit();

    // this may fail if we are not in focus
    if (!backend->pointers.empty() && cursorState.serial)
        backend->pointers.at(0)->pointer->sendSetCursor(cursorState.serial, cursorState.cursorSurface.get(), cursorState.hotspot.x, cursorState.hotspot.y);

    return true;
}

void Aquamarine::CWaylandOutput::moveCursor(const Hyprutils::Math::Vector2D& coord, bool skipSchedule) {
    ;
}

void Aquamarine::CWaylandOutput::onEnter(SP<CCWlPointer> pointer, uint32_t serial) {
    cursorState.serial = serial;

    if (!cursorState.cursorSurface)
        return;

    pointer->sendSetCursor(serial, cursorState.cursorSurface.get(), cursorState.hotspot.x, cursorState.hotspot.y);
}

Hyprutils::Math::Vector2D Aquamarine::CWaylandOutput::cursorPlaneSize() {
    return {-1, -1}; // no limit
}

void Aquamarine::CWaylandOutput::scheduleFrame(const scheduleFrameReason reason) {
    TRACE(backend->backend->log(AQ_LOG_TRACE,
                                std::format("CWaylandOutput::scheduleFrame: reason {}, needsFrame {}, frameScheduled {}", (uint32_t)reason, needsFrame, frameScheduled)));
    needsFrame = true;

    if (frameScheduled)
        return;

    frameScheduled = true;

    if (waylandState.frameCallback)
        frameScheduledWhileWaiting = true;
    else {
        backend->idleCallbacks.emplace_back([w = self]() {
            if (auto o = w.lock())
                o->sendFrameAndSetCallback();
        });
    }
}

Aquamarine::CWaylandBuffer::CWaylandBuffer(SP<IBuffer> buffer_, Hyprutils::Memory::CWeakPointer<CWaylandBackend> backend_) : buffer(buffer_), backend(backend_) {
    auto shmAttrs = buffer_->shm();
    if (backend->shmMode || shmAttrs.success) {
        if (!shmAttrs.success) {
            backend->backend->log(AQ_LOG_ERROR, "WaylandBuffer: shm mode but buffer is not a shm buffer");
            return;
        }

        const size_t poolLen = (size_t)shmAttrs.stride * (size_t)shmAttrs.size.y;

        waylandState.pool = makeShared<CCWlShmPool>(backend->waylandState.shm->sendCreatePool(shmAttrs.fd, poolLen));
        if (!waylandState.pool) {
            backend->backend->log(AQ_LOG_ERROR, "WaylandBuffer: failed to create a wl_shm pool");
            return;
        }

        waylandState.buffer =
            makeShared<CCWlBuffer>(waylandState.pool->sendCreateBuffer(0, shmAttrs.size.x, shmAttrs.size.y, shmAttrs.stride, shmFormatFromDRM(shmAttrs.format)));

        if (!waylandState.buffer) {
            backend->backend->log(AQ_LOG_ERROR, "WaylandBuffer: failed to create a wl_shm wl_buffer");
            return;
        }

        waylandState.buffer->setRelease([this](CCWlBuffer* r) { pendingRelease = false; });
        return;
    }

    auto params = makeShared<CCZwpLinuxBufferParamsV1>(backend->waylandState.dmabuf->sendCreateParams());

    if (!params) {
        backend->backend->log(AQ_LOG_ERROR, "WaylandBuffer: failed to query params");
        return;
    }

    auto attrs = buffer->dmabuf();

    for (int i = 0; i < attrs.planes; ++i) {
        params->sendAdd(attrs.fds.at(i), i, attrs.offsets.at(i), attrs.strides.at(i), attrs.modifier >> 32, attrs.modifier & 0xFFFFFFFF);
    }

    waylandState.buffer = makeShared<CCWlBuffer>(params->sendCreateImmed(attrs.size.x, attrs.size.y, attrs.format, (zwpLinuxBufferParamsV1Flags)0));

    waylandState.buffer->setRelease([this](CCWlBuffer* r) { pendingRelease = false; });

    params->sendDestroy();
}

Aquamarine::CWaylandBuffer::~CWaylandBuffer() {
    if (waylandState.buffer && waylandState.buffer->resource())
        waylandState.buffer->sendDestroy();
    if (waylandState.pool && waylandState.pool->resource())
        waylandState.pool->sendDestroy();
}

bool Aquamarine::CWaylandBuffer::good() {
    return waylandState.buffer && waylandState.buffer->resource();
}
