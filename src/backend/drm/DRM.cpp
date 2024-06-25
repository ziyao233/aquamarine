#include <aquamarine/backend/DRM.hpp>
#include <aquamarine/backend/drm/Legacy.hpp>
#include <chrono>
#include <thread>
#include <deque>
#include <cstring>
#include <sys/mman.h>

extern "C" {
#include <libseat.h>
#include <libudev.h>
#include <xf86drm.h>
#include <xf86drmMode.h>
#include <libdisplay-info/cvt.h>
}

#include "Props.hpp"
#include "FormatUtils.hpp"

using namespace Aquamarine;
using namespace Hyprutils::Memory;
using namespace Hyprutils::Math;
#define SP CSharedPointer

Aquamarine::CDRMBackend::CDRMBackend(SP<CBackend> backend_) : backend(backend_) {
    listeners.sessionActivate = backend->session->events.changeActive.registerListener([this](std::any d) {
        if (backend->session->active) {
            // session got activated, we need to restore
            restoreAfterVT();
        }
    });
}

static udev_enumerate* enumDRMCards(udev* udev) {
    auto enumerate = udev_enumerate_new(udev);
    if (!enumerate)
        return nullptr;

    udev_enumerate_add_match_subsystem(enumerate, "drm");
    udev_enumerate_add_match_sysname(enumerate, DRM_PRIMARY_MINOR_NAME "[0-9]*");

    if (udev_enumerate_scan_devices(enumerate)) {
        udev_enumerate_unref(enumerate);
        return nullptr;
    }

    return enumerate;
}

static std::vector<SP<CSessionDevice>> scanGPUs(SP<CBackend> backend) {
    // FIXME: This provides no explicit way to set a preferred gpu

    auto enumerate = enumDRMCards(backend->session->udevHandle);

    if (!enumerate) {
        backend->log(AQ_LOG_ERROR, "drm: couldn't enumerate gpus with udev");
        return {};
    }

    if (!udev_enumerate_get_list_entry(enumerate)) {
        // TODO: wait for them.
        backend->log(AQ_LOG_ERROR, "drm: No gpus in scanGPUs.");
        return {};
    }

    udev_list_entry*               entry = nullptr;
    size_t                         i     = 0;

    std::deque<SP<CSessionDevice>> devices;

    udev_list_entry_foreach(entry, udev_enumerate_get_list_entry(enumerate)) {
        auto path   = udev_list_entry_get_name(entry);
        auto device = udev_device_new_from_syspath(backend->session->udevHandle, path);
        if (!device) {
            backend->log(AQ_LOG_WARNING, std::format("drm: Skipping device {}", path ? path : "unknown"));
            continue;
        }

        backend->log(AQ_LOG_DEBUG, std::format("drm: Enumerated device {}", path ? path : "unknown"));

        auto seat = udev_device_get_property_value(device, "ID_SEAT");
        if (!seat)
            seat = "seat0";

        if (!backend->session->seatName.empty() && backend->session->seatName != seat) {
            backend->log(AQ_LOG_WARNING, std::format("drm: Skipping device {} because seat {} doesn't match our {}", path ? path : "unknown", seat, backend->session->seatName));
            udev_device_unref(device);
            continue;
        }

        auto pciDevice = udev_device_get_parent_with_subsystem_devtype(device, "pci", nullptr);
        bool isBootVGA = false;
        if (pciDevice) {
            auto id   = udev_device_get_sysattr_value(pciDevice, "boot_vga");
            isBootVGA = id && id == std::string{"1"};
        }

        if (!udev_device_get_devnode(device)) {
            backend->log(AQ_LOG_ERROR, std::format("drm: Skipping device {}, no devnode", path ? path : "unknown"));
            udev_device_unref(device);
            continue;
        }

        auto sessionDevice = CSessionDevice::openIfKMS(backend->session, udev_device_get_devnode(device));
        if (!sessionDevice) {
            backend->log(AQ_LOG_ERROR, std::format("drm: Skipping device {}, not a KMS device", path ? path : "unknown"));
            udev_device_unref(device);
            continue;
        }

        udev_device_unref(device);

        if (isBootVGA)
            devices.push_front(sessionDevice);
        else
            devices.push_back(sessionDevice);

        ++i;
    }

    udev_enumerate_unref(enumerate);

    std::vector<SP<CSessionDevice>> vecDevices;
    for (auto& d : devices) {
        vecDevices.push_back(d);
    }

    return vecDevices;
}

SP<CDRMBackend> Aquamarine::CDRMBackend::attempt(SP<CBackend> backend) {
    if (!backend->session)
        backend->session = CSession::attempt(backend);

    if (!backend->session) {
        backend->log(AQ_LOG_ERROR, "Failed to open a session");
        return nullptr;
    }

    if (!backend->session->active) {
        backend->log(AQ_LOG_DEBUG, "Session is not active, waiting for 5s");

        auto started = std::chrono::system_clock::now();

        while (!backend->session->active) {
            std::this_thread::sleep_for(std::chrono::milliseconds(250));
            backend->session->dispatchPendingEventsAsync();

            if (std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now() - started).count() >= 5000) {
                backend->log(AQ_LOG_DEBUG, "Session timeout reached");
                break;
            }
        }

        if (!backend->session->active) {
            backend->log(AQ_LOG_DEBUG, "Session could not be activated in time");
            return nullptr;
        }
    }

    auto gpus = scanGPUs(backend);

    if (gpus.empty()) {
        backend->log(AQ_LOG_ERROR, "drm: Found no gpus to use, cannot continue");
        return nullptr;
    }

    backend->log(AQ_LOG_DEBUG, std::format("drm: Found {} GPUs", gpus.size()));

    // FIXME: this will ignore multi-gpu setups and only create one backend
    auto drmBackend  = SP<CDRMBackend>(new CDRMBackend(backend));
    drmBackend->self = drmBackend;

    if (!drmBackend->registerGPU(gpus.at(0))) {
        backend->log(AQ_LOG_ERROR, std::format("drm: Failed to register gpu at fd {}", gpus[0]->fd));
        return nullptr;
    } else
        backend->log(AQ_LOG_DEBUG, std::format("drm: Registered gpu at fd {}", gpus[0]->fd));

    // TODO: consider listening for new devices
    // But if you expect me to handle gpu hotswaps you are probably insane LOL

    if (!drmBackend->checkFeatures()) {
        backend->log(AQ_LOG_ERROR, "drm: Failed checking features");
        return nullptr;
    }

    if (!drmBackend->initResources()) {
        backend->log(AQ_LOG_ERROR, "drm: Failed initializing resources");
        return nullptr;
    }

    backend->log(AQ_LOG_DEBUG, std::format("drm: Basic init pass for gpu {}", gpus[0]->path));

    drmBackend->grabFormats();

    drmBackend->scanConnectors();

    return drmBackend;
}

Aquamarine::CDRMBackend::~CDRMBackend() {
    ;
}

void Aquamarine::CDRMBackend::log(eBackendLogLevel l, const std::string& s) {
    backend->log(l, s);
}

bool Aquamarine::CDRMBackend::sessionActive() {
    return backend->session->active;
}

void Aquamarine::CDRMBackend::restoreAfterVT() {
    backend->log(AQ_LOG_DEBUG, "drm: Restoring after VT switch");

    scanConnectors();

    backend->log(AQ_LOG_DEBUG, "drm: Rescanned connectors");

    for (auto& c : connectors) {
        if (!c->crtc)
            continue;

        backend->log(AQ_LOG_DEBUG, std::format("drm: Resetting crtc {}", c->crtc->id));

        if (!impl->reset(c))
            backend->log(AQ_LOG_ERROR, std::format("drm: crtc {} failed reset", c->crtc->id));
    }

    for (auto& c : connectors) {
        if (!c->crtc)
            continue;

        SDRMConnectorCommitData data = {
            .mainFB   = nullptr,
            .modeset  = true,
            .blocking = true,
            .flags    = 0,
            .test     = false,
        };

        if (c->output->state->state().mode && c->output->state->state().mode->modeInfo.has_value())
            data.modeInfo = *c->output->state->state().mode->modeInfo;
        else
            data.calculateMode(c);

        backend->log(AQ_LOG_DEBUG,
                     std::format("drm: Restoring crtc {} with clock {} hdisplay {} vdisplay {} vrefresh {}", c->crtc->id, data.modeInfo.clock, data.modeInfo.hdisplay,
                                 data.modeInfo.vdisplay, data.modeInfo.vrefresh));

        if (!impl->commit(c, data))
            backend->log(AQ_LOG_ERROR, std::format("drm: crtc {} failed restore", c->crtc->id));
    }
}

bool Aquamarine::CDRMBackend::checkFeatures() {
    uint64_t curW = 0, curH = 0;
    if (drmGetCap(gpu->fd, DRM_CAP_CURSOR_WIDTH, &curW))
        curW = 64;
    if (drmGetCap(gpu->fd, DRM_CAP_CURSOR_HEIGHT, &curH))
        curH = 64;

    drmProps.cursorSize = Hyprutils::Math::Vector2D{(double)curW, (double)curH};

    uint64_t cap = 0;
    if (drmGetCap(gpu->fd, DRM_CAP_PRIME, &cap) || !(cap & DRM_PRIME_CAP_IMPORT)) {
        backend->log(AQ_LOG_ERROR, std::format("drm: DRM_PRIME_CAP_IMPORT unsupported"));
        return false;
    }

    if (drmGetCap(gpu->fd, DRM_CAP_CRTC_IN_VBLANK_EVENT, &cap) || !cap) {
        backend->log(AQ_LOG_ERROR, std::format("drm: DRM_CAP_CRTC_IN_VBLANK_EVENT unsupported"));
        return false;
    }

    if (drmGetCap(gpu->fd, DRM_CAP_TIMESTAMP_MONOTONIC, &cap) || !cap) {
        backend->log(AQ_LOG_ERROR, std::format("drm: DRM_PRIME_CAP_IMPORT unsupported"));
        return false;
    }

    if (drmSetClientCap(gpu->fd, DRM_CLIENT_CAP_UNIVERSAL_PLANES, 1)) {
        backend->log(AQ_LOG_ERROR, std::format("drm: DRM_CLIENT_CAP_UNIVERSAL_PLANES unsupported"));
        return false;
    }

    drmProps.supportsAsyncCommit     = drmGetCap(gpu->fd, DRM_CAP_ASYNC_PAGE_FLIP, &cap) == 0 && cap == 1;
    drmProps.supportsAddFb2Modifiers = drmGetCap(gpu->fd, DRM_CAP_ADDFB2_MODIFIERS, &cap) == 0 && cap == 1;

    backend->log(AQ_LOG_DEBUG, std::format("drm: drmProps.supportsAsyncCommit: {}", drmProps.supportsAsyncCommit));
    backend->log(AQ_LOG_DEBUG, std::format("drm: drmProps.supportsAddFb2Modifiers: {}", drmProps.supportsAddFb2Modifiers));

    impl = makeShared<CDRMLegacyImpl>(self.lock());

    // TODO: allow no-modifiers?

    return true;
}

bool Aquamarine::CDRMBackend::initResources() {
    auto resources = drmModeGetResources(gpu->fd);
    if (!resources) {
        backend->log(AQ_LOG_ERROR, std::format("drm: drmModeGetResources failed"));
        return false;
    }

    backend->log(AQ_LOG_DEBUG, std::format("drm: found {} CRTCs", resources->count_crtcs));

    for (size_t i = 0; i < resources->count_crtcs; ++i) {
        auto CRTC     = makeShared<SDRMCRTC>();
        CRTC->id      = resources->crtcs[i];
        CRTC->backend = self;

        auto drmCRTC = drmModeGetCrtc(gpu->fd, CRTC->id);
        if (!drmCRTC) {
            backend->log(AQ_LOG_ERROR, std::format("drm: drmModeGetCrtc for crtc {} failed", CRTC->id));
            drmModeFreeResources(resources);
            crtcs.clear();
            return false;
        }

        CRTC->legacy.gammaSize = drmCRTC->gamma_size;
        drmModeFreeCrtc(drmCRTC);

        if (!getDRMCRTCProps(gpu->fd, CRTC->id, &CRTC->props)) {
            backend->log(AQ_LOG_ERROR, std::format("drm: getDRMCRTCProps for crtc {} failed", CRTC->id));
            drmModeFreeResources(resources);
            crtcs.clear();
            return false;
        }

        crtcs.emplace_back(CRTC);
    }

    if (crtcs.size() > 32) {
        backend->log(AQ_LOG_CRITICAL, "drm: Cannot support more than 32 CRTCs");
        return false;
    }

    // initialize planes
    auto planeResources = drmModeGetPlaneResources(gpu->fd);
    if (!planeResources) {
        backend->log(AQ_LOG_ERROR, std::format("drm: drmModeGetPlaneResources failed"));
        return false;
    }

    backend->log(AQ_LOG_DEBUG, std::format("drm: found {} planes", planeResources->count_planes));

    for (uint32_t i = 0; i < planeResources->count_planes; ++i) {
        auto id    = planeResources->planes[i];
        auto plane = drmModeGetPlane(gpu->fd, id);
        if (!plane) {
            backend->log(AQ_LOG_ERROR, std::format("drm: drmModeGetPlane for plane {} failed", id));
            drmModeFreeResources(resources);
            crtcs.clear();
            planes.clear();
            return false;
        }

        auto aqPlane     = makeShared<SDRMPlane>();
        aqPlane->backend = self;
        aqPlane->self    = aqPlane;
        if (!aqPlane->init((drmModePlane*)plane)) {
            backend->log(AQ_LOG_ERROR, std::format("drm: aqPlane->init for plane {} failed", id));
            drmModeFreeResources(resources);
            crtcs.clear();
            planes.clear();
            return false;
        }

        planes.emplace_back(aqPlane);

        drmModeFreePlane(plane);
    }

    drmModeFreePlaneResources(planeResources);
    drmModeFreeResources(resources);

    return true;
}

bool Aquamarine::CDRMBackend::grabFormats() {
    // FIXME: do this properly maybe?
    return true;
}

bool Aquamarine::CDRMBackend::registerGPU(SP<CSessionDevice> gpu_, SP<CDRMBackend> primary_) {
    gpu     = gpu_;
    primary = primary_;

    auto drmName = drmGetDeviceNameFromFd2(gpu->fd);
    auto drmVer  = drmGetVersion(gpu->fd);

    gpuName = drmName;

    backend->log(AQ_LOG_DEBUG, std::format("drm: Starting backend for {}, with driver {}", drmName ? drmName : "unknown", drmVer->name ? drmVer->name : "unknown"));

    drmFreeVersion(drmVer);

    listeners.gpuChange = gpu->events.change.registerListener([this](std::any d) {
        auto E = std::any_cast<CSessionDevice::SChangeEvent>(d);
        if (E.type == CSessionDevice::AQ_SESSION_EVENT_CHANGE_HOTPLUG) {
            backend->log(AQ_LOG_DEBUG, std::format("drm: Got a hotplug event for {}", gpuName));
            scanConnectors();
        }
    });

    listeners.gpuRemove = gpu->events.remove.registerListener(
        [this](std::any d) { backend->log(AQ_LOG_ERROR, std::format("drm: !!!!FIXME: Got a remove event for {}, this is not handled properly!!!!!", gpuName)); });

    return true;
}

eBackendType Aquamarine::CDRMBackend::type() {
    return eBackendType::AQ_BACKEND_DRM;
}

void Aquamarine::CDRMBackend::scanConnectors() {
    backend->log(AQ_LOG_DEBUG, std::format("drm: Scanning connectors for {}", gpu->path));

    auto resources = drmModeGetResources(gpu->fd);
    if (!resources) {
        backend->log(AQ_LOG_ERROR, std::format("drm: Scanning connectors for {} failed", gpu->path));
        return;
    }

    for (size_t i = 0; i < resources->count_connectors; ++i) {
        uint32_t          connectorID = resources->connectors[i];

        SP<SDRMConnector> conn;
        auto              drmConn = drmModeGetConnector(gpu->fd, connectorID);

        backend->log(AQ_LOG_DEBUG, std::format("drm: Scanning connector id {}", connectorID));

        if (!drmConn) {
            backend->log(AQ_LOG_ERROR, std::format("drm: Failed to get connector id {}", connectorID));
            continue;
        }

        auto it = std::find_if(connectors.begin(), connectors.end(), [connectorID](const auto& e) { return e->id == connectorID; });
        if (it == connectors.end()) {
            backend->log(AQ_LOG_DEBUG, std::format("drm: Initializing connector id {}", connectorID));
            conn          = connectors.emplace_back(SP<SDRMConnector>(new SDRMConnector()));
            conn->self    = conn;
            conn->backend = self;
            if (!conn->init(drmConn)) {
                backend->log(AQ_LOG_ERROR, std::format("drm: Connector id {} failed initializing", connectorID));
                connectors.pop_back();
                continue;
            }
        } else
            conn = *it;

        backend->log(AQ_LOG_DEBUG, std::format("drm: Connectors size {}", connectors.size()));

        backend->log(AQ_LOG_DEBUG, std::format("drm: Connector {} connection state: {}", connectorID, (int)drmConn->connection));

        if (conn->status == DRM_MODE_DISCONNECTED && drmConn->connection == DRM_MODE_CONNECTED) {
            backend->log(AQ_LOG_DEBUG, std::format("drm: Connector {} connected", conn->szName));
            conn->connect(drmConn);
        } else if (conn->status == DRM_MODE_CONNECTED && drmConn->connection == DRM_MODE_DISCONNECTED) {
            backend->log(AQ_LOG_DEBUG, std::format("drm: Connector {} disconnected", conn->szName));
            conn->disconnect();
        }

        drmModeFreeConnector(drmConn);
    }

    drmModeFreeResources(resources);
}

bool Aquamarine::CDRMBackend::start() {
    return true;
}

int Aquamarine::CDRMBackend::pollFD() {
    return gpu->fd;
}

int Aquamarine::CDRMBackend::drmFD() {
    return gpu->fd;
}

static void handlePF(int fd, unsigned seq, unsigned tv_sec, unsigned tv_usec, unsigned crtc_id, void* data) {
    auto pageFlip = (SDRMPageFlip*)data;

    if (!pageFlip->connector)
        return;

    pageFlip->connector->isPageFlipPending = false;

    const auto& BACKEND = pageFlip->connector->backend;

    BACKEND->log(AQ_LOG_TRACE, std::format("drm: pf event seq {} sec {} usec {} crtc {}", seq, tv_sec, tv_usec, crtc_id));

    if (pageFlip->connector->status != DRM_MODE_CONNECTED || !pageFlip->connector->crtc) {
        BACKEND->log(AQ_LOG_DEBUG, "drm: Ignoring a pf event from a disabled crtc / connector");
        return;
    }

    pageFlip->connector->onPresent();

    uint32_t flags = IOutput::AQ_OUTPUT_PRESENT_VSYNC | IOutput::AQ_OUTPUT_PRESENT_HW_CLOCK | IOutput::AQ_OUTPUT_PRESENT_HW_COMPLETION | IOutput::AQ_OUTPUT_PRESENT_ZEROCOPY;

    timespec presented = {.tv_sec = tv_sec, .tv_nsec = tv_usec * 1000};

    pageFlip->connector->output->events.present.emit(IOutput::SPresentEvent{
        .presented = BACKEND->sessionActive(),
        .when      = &presented,
        .seq       = seq,
        .refresh   = (int)(pageFlip->connector->refresh ? (1000000000000LL / pageFlip->connector->refresh) : 0),
        .flags     = flags,
    });

    if (BACKEND->sessionActive())
        pageFlip->connector->output->events.frame.emit();
}

bool Aquamarine::CDRMBackend::dispatchEvents() {
    drmEventContext event = {
        .version            = 3,
        .page_flip_handler2 = ::handlePF,
    };

    if (drmHandleEvent(gpu->fd, &event) != 0)
        backend->log(AQ_LOG_ERROR, std::format("drm: Failed to handle event on fd {}", gpu->fd));

    if (!idleCallbacks.empty()) {
        for (auto& c : idleCallbacks) {
            c();
        }
        idleCallbacks.clear();
    }

    return true;
}

uint32_t Aquamarine::CDRMBackend::capabilities() {
    return eBackendCapabilities::AQ_BACKEND_CAPABILITY_POINTER;
}

bool Aquamarine::CDRMBackend::setCursor(SP<IBuffer> buffer, const Hyprutils::Math::Vector2D& hotspot) {
    return false;
}

void Aquamarine::CDRMBackend::onReady() {
    backend->log(AQ_LOG_DEBUG, std::format("drm: Connectors size2 {}", connectors.size()));

    for (auto& c : connectors) {
        backend->log(AQ_LOG_DEBUG, std::format("drm: onReady: connector {}", c->id));
        if (!c->output)
            continue;

        backend->log(AQ_LOG_DEBUG, std::format("drm: onReady: connector {} has output name {}", c->id, c->output->name));

        // swapchain has to be created here because allocator is absent in connect if not ready
        c->output->swapchain = makeShared<CSwapchain>(backend->allocator);
        c->output->swapchain->reconfigure(SSwapchainOptions{.length = 0, .scanout = true}); // mark the swapchain for scanout
        c->output->needsFrame = true;

        backend->events.newOutput.emit(SP<IOutput>(c->output));
    }
}

std::vector<SDRMFormat> Aquamarine::CDRMBackend::getRenderFormats() {
    for (auto& p : planes) {
        if (p->type != DRM_PLANE_TYPE_PRIMARY)
            continue;

        return p->formats;
    }

    return {};
}

std::vector<SDRMFormat> Aquamarine::CDRMBackend::getCursorFormats() {
    for (auto& p : planes) {
        if (p->type != DRM_PLANE_TYPE_CURSOR)
            continue;

        return p->formats;
    }

    return {};
}

bool Aquamarine::SDRMPlane::init(drmModePlane* plane) {
    id = plane->plane_id;

    if (!getDRMPlaneProps(backend->gpu->fd, id, &props))
        return false;

    if (!getDRMProp(backend->gpu->fd, id, props.type, &type))
        return false;

    initialID = id;

    backend->backend->log(AQ_LOG_DEBUG, std::format("drm: Plane {} has type {}", id, (int)type));

    backend->backend->log(AQ_LOG_DEBUG, std::format("drm: Plane {} has {} formats", id, plane->count_formats));

    for (size_t i = 0; i < plane->count_formats; ++i) {
        if (type != DRM_PLANE_TYPE_CURSOR)
            formats.emplace_back(SDRMFormat{.drmFormat = plane->formats[i], .modifiers = {DRM_FORMAT_MOD_LINEAR, DRM_FORMAT_MOD_INVALID}});
        else
            formats.emplace_back(SDRMFormat{.drmFormat = plane->formats[i], .modifiers = {DRM_FORMAT_MOD_LINEAR}});

        backend->backend->log(AQ_LOG_TRACE, std::format("drm: | Format {}", fourccToName(plane->formats[i])));
    }

    if (props.in_formats && backend->drmProps.supportsAddFb2Modifiers) {
        backend->backend->log(AQ_LOG_DEBUG, "drm: Plane: checking for modifiers");

        uint64_t blobID = 0;
        if (!getDRMProp(backend->gpu->fd, id, props.in_formats, &blobID)) {
            backend->backend->log(AQ_LOG_ERROR, "drm: Plane: No blob id");
            return false;
        }

        auto blob = drmModeGetPropertyBlob(backend->gpu->fd, blobID);
        if (!blob) {
            backend->backend->log(AQ_LOG_ERROR, "drm: Plane: No property");
            return false;
        }

        drmModeFormatModifierIterator iter = {0};
        while (drmModeFormatModifierBlobIterNext(blob, &iter)) {
            auto it = std::find_if(formats.begin(), formats.end(), [iter](const auto& e) { return e.drmFormat == iter.fmt; });

            backend->backend->log(AQ_LOG_TRACE, std::format("drm: | Modifier {} with format {}", iter.mod, fourccToName(iter.fmt)));

            if (it == formats.end())
                formats.emplace_back(SDRMFormat{.drmFormat = iter.fmt, .modifiers = {iter.mod}});
            else
                it->modifiers.emplace_back(iter.mod);
        }

        drmModeFreePropertyBlob(blob);
    }

    for (size_t i = 0; i < backend->crtcs.size(); ++i) {
        uint32_t crtcBit = (1 << i);
        if (!(plane->possible_crtcs & crtcBit))
            continue;

        auto CRTC = backend->crtcs.at(i);
        if (type == DRM_PLANE_TYPE_PRIMARY && !CRTC->primary) {
            CRTC->primary = self.lock();
            break;
        }

        if (type == DRM_PLANE_TYPE_CURSOR && !CRTC->cursor) {
            CRTC->cursor = self.lock();
            break;
        }
    }

    return true;
}

SP<SDRMCRTC> Aquamarine::SDRMConnector::getCurrentCRTC(const drmModeConnector* connector) {
    uint32_t crtcID = 0;
    if (props.crtc_id) {
        uint64_t value = 0;
        if (!getDRMProp(backend->gpu->fd, id, props.crtc_id, &value)) {
            backend->backend->log(AQ_LOG_ERROR, "drm: Failed to get CRTC_ID");
            return nullptr;
        }
        crtcID = static_cast<uint32_t>(value);
    } else if (connector->encoder_id) {
        auto encoder = drmModeGetEncoder(backend->gpu->fd, connector->encoder_id);
        if (!encoder) {
            backend->backend->log(AQ_LOG_ERROR, "drm: drmModeGetEncoder failed");
            return nullptr;
        }
        crtcID = encoder->crtc_id;
        drmModeFreeEncoder(encoder);
    } else
        return nullptr;

    auto it = std::find_if(backend->crtcs.begin(), backend->crtcs.end(), [crtcID](const auto& e) { return e->id == crtcID; });

    if (it == backend->crtcs.end()) {
        backend->backend->log(AQ_LOG_ERROR, std::format("drm: Failed to find a CRTC with ID {}", crtcID));
        return nullptr;
    }

    return *it;
}

bool Aquamarine::SDRMConnector::init(drmModeConnector* connector) {
    id                        = connector->connector_id;
    pendingPageFlip.connector = self.lock();

    if (!getDRMConnectorProps(backend->gpu->fd, id, &props))
        return false;

    auto name = drmModeGetConnectorTypeName(connector->connector_type);
    if (!name)
        name = "ERROR";

    szName = std::format("{}-{}", name, connector->connector_type_id);

    possibleCrtcs = drmModeConnectorGetPossibleCrtcs(backend->gpu->fd, connector);
    if (!possibleCrtcs)
        backend->backend->log(AQ_LOG_ERROR, "drm: No CRTCs possible");

    crtc = getCurrentCRTC(connector);

    return true;
}

Aquamarine::SDRMConnector::~SDRMConnector() {
    disconnect();
}

static int32_t calculateRefresh(const drmModeModeInfo& mode) {
    int32_t refresh = (mode.clock * 1000000LL / mode.htotal + mode.vtotal / 2) / mode.vtotal;

    if (mode.flags & DRM_MODE_FLAG_INTERLACE)
        refresh *= 2;

    if (mode.flags & DRM_MODE_FLAG_DBLSCAN)
        refresh /= 2;

    if (mode.vscan > 1)
        refresh /= mode.vscan;

    return refresh;
}

drmModeModeInfo* Aquamarine::SDRMConnector::getCurrentMode() {
    if (!crtc)
        return nullptr;

    if (crtc->props.mode_id) {
        size_t size = 0;
        return (drmModeModeInfo*)getDRMPropBlob(backend->gpu->fd, crtc->id, crtc->props.mode_id, &size);
        ;
    }

    auto drmCrtc = drmModeGetCrtc(backend->gpu->fd, crtc->id);
    if (!drmCrtc)
        return nullptr;
    if (!drmCrtc->mode_valid) {
        drmModeFreeCrtc(drmCrtc);
        return nullptr;
    }

    drmModeModeInfo* modeInfo = (drmModeModeInfo*)malloc(sizeof(drmModeModeInfo));
    if (!modeInfo) {
        drmModeFreeCrtc(drmCrtc);
        return nullptr;
    }

    *modeInfo = drmCrtc->mode;
    drmModeFreeCrtc(drmCrtc);

    return modeInfo;
}

void Aquamarine::SDRMConnector::parseEDID(std::vector<uint8_t> data) {
    // TODO: libdisplay-info prolly
}

void Aquamarine::SDRMConnector::connect(drmModeConnector* connector) {
    if (output) {
        backend->backend->log(AQ_LOG_DEBUG, std::format("drm: Not connecting connector {} because it's already connected", szName));
        return;
    }

    backend->backend->log(AQ_LOG_DEBUG, std::format("drm: Connecting connector {}, CRTC ID {}", szName, crtc ? crtc->id : -1));

    output            = SP<CDRMOutput>(new CDRMOutput(szName, backend, self.lock()));
    output->self      = output;
    output->connector = self.lock();

    backend->backend->log(AQ_LOG_DEBUG, "drm: Dumping detected modes:");

    auto currentModeInfo = getCurrentMode();

    for (int i = 0; i < connector->count_modes; ++i) {
        auto& drmMode = connector->modes[i];

        if (drmMode.flags & DRM_MODE_FLAG_INTERLACE) {
            backend->backend->log(AQ_LOG_DEBUG, std::format("drm: Skipping mode {} because it's interlaced", i));
            continue;
        }

        if (i == 1)
            fallbackModeInfo = drmMode;

        auto aqMode         = makeShared<SOutputMode>();
        aqMode->pixelSize   = {drmMode.hdisplay, drmMode.vdisplay};
        aqMode->refreshRate = calculateRefresh(drmMode);
        aqMode->preferred   = (drmMode.type & DRM_MODE_TYPE_PREFERRED);
        aqMode->modeInfo    = drmMode;

        output->modes.emplace_back(aqMode);

        if (currentModeInfo && std::memcmp(&drmMode, currentModeInfo, sizeof(drmModeModeInfo))) {
            output->state->setMode(aqMode);

            //uint64_t modeID = 0;
            // getDRMProp(backend->gpu->fd, crtc->id, crtc->props.mode_id, &modeID);

            crtc->refresh = calculateRefresh(drmMode);
        }

        backend->backend->log(AQ_LOG_DEBUG,
                              std::format("drm: Mode {}: {}x{}@{:.2f}Hz {}", i, (int)aqMode->pixelSize.x, (int)aqMode->pixelSize.y, aqMode->refreshRate / 1000.0,
                                          aqMode->preferred ? " (preferred)" : ""));
    }

    output->physicalSize = {(double)connector->mmWidth, (double)connector->mmHeight};

    backend->backend->log(AQ_LOG_DEBUG, std::format("drm: Physical size {} (mm)", output->physicalSize));

    switch (connector->subpixel) {
        case DRM_MODE_SUBPIXEL_NONE: output->subpixel = eSubpixelMode::AQ_SUBPIXEL_NONE; break;
        case DRM_MODE_SUBPIXEL_UNKNOWN: output->subpixel = eSubpixelMode::AQ_SUBPIXEL_UNKNOWN; break;
        case DRM_MODE_SUBPIXEL_HORIZONTAL_RGB: output->subpixel = eSubpixelMode::AQ_SUBPIXEL_HORIZONTAL_RGB; break;
        case DRM_MODE_SUBPIXEL_HORIZONTAL_BGR: output->subpixel = eSubpixelMode::AQ_SUBPIXEL_HORIZONTAL_BGR; break;
        case DRM_MODE_SUBPIXEL_VERTICAL_RGB: output->subpixel = eSubpixelMode::AQ_SUBPIXEL_VERTICAL_RGB; break;
        case DRM_MODE_SUBPIXEL_VERTICAL_BGR: output->subpixel = eSubpixelMode::AQ_SUBPIXEL_VERTICAL_BGR; break;
        default: output->subpixel = eSubpixelMode::AQ_SUBPIXEL_UNKNOWN;
    }

    uint64_t prop = 0;
    if (getDRMProp(backend->gpu->fd, id, props.non_desktop, &prop)) {
        if (prop == 1)
            backend->backend->log(AQ_LOG_DEBUG, "drm: Non-desktop connector");
        output->nonDesktop = prop;
    }

    canDoVrr           = props.vrr_capable && crtc->props.vrr_enabled && !getDRMProp(backend->gpu->fd, id, props.vrr_capable, &prop) && prop;
    output->vrrCapable = canDoVrr;

    maxBpcBounds.fill(0);

    if (props.max_bpc && !introspectDRMPropRange(backend->gpu->fd, props.max_bpc, maxBpcBounds.data(), &maxBpcBounds[1]))
        backend->backend->log(AQ_LOG_ERROR, "drm: Failed to check max_bpc");

    size_t               edidLen  = 0;
    uint8_t*             edidData = (uint8_t*)getDRMPropBlob(backend->gpu->fd, id, props.edid, &edidLen);

    std::vector<uint8_t> edid{edidData, edidData + edidLen};
    parseEDID(edid);

    free(edidData);
    edid = {};

    // TODO: subconnectors

    output->make        = make;
    output->model       = model;
    output->serial      = serial;
    output->description = std::format("{} {} {} ({})", make, model, serial, szName);
    output->needsFrame  = true;

    backend->backend->log(AQ_LOG_DEBUG, std::format("drm: Description {}", output->description));

    status = DRM_MODE_CONNECTED;

    if (!backend->backend->ready)
        return;

    output->swapchain = makeShared<CSwapchain>(backend->backend->allocator);
    backend->backend->events.newOutput.emit(output);
    output->scheduleFrame();
}

void Aquamarine::SDRMConnector::disconnect() {
    if (!output) {
        backend->backend->log(AQ_LOG_DEBUG, std::format("drm: Not disconnecting connector {} because it's already disconnected", szName));
        return;
    }

    output->events.destroy.emit();
    output.reset();

    status = DRM_MODE_DISCONNECTED;
}

bool Aquamarine::SDRMConnector::commitState(const SDRMConnectorCommitData& data) {
    const bool ok = backend->impl->commit(self.lock(), data);

    if (ok && !data.test)
        applyCommit(data);
    else
        rollbackCommit(data);

    return ok;
}

void Aquamarine::SDRMConnector::applyCommit(const SDRMConnectorCommitData& data) {
    crtc->primary->back  = crtc->primary->front;
    crtc->primary->front = data.mainFB;
    if (crtc->cursor) {
        crtc->cursor->back  = crtc->cursor->front;
        crtc->cursor->front = data.cursorFB;
    }

    pendingCursorFB.reset();

    if (output->state->state().committed & COutputState::AQ_OUTPUT_STATE_MODE)
        refresh = calculateRefresh(data.modeInfo);
}

void Aquamarine::SDRMConnector::rollbackCommit(const SDRMConnectorCommitData& data) {
    ;
}

void Aquamarine::SDRMConnector::onPresent() {
    ;
}

Aquamarine::CDRMOutput::~CDRMOutput() {
    ;
}

bool Aquamarine::CDRMOutput::commit() {
    return commitState();
}

bool Aquamarine::CDRMOutput::test() {
    return commitState(true);
}

bool Aquamarine::CDRMOutput::commitState(bool onlyTest) {
    if (!backend->backend->session->active) {
        backend->backend->log(AQ_LOG_ERROR, "drm: Session inactive");
        return false;
    }

    if (!connector->crtc) {
        backend->backend->log(AQ_LOG_ERROR, "drm: No CRTC attached to output");
        return false;
    }

    const auto&    STATE     = state->state();
    const uint32_t COMMITTED = STATE.committed;

    if ((COMMITTED & COutputState::eOutputStateProperties::AQ_OUTPUT_STATE_ENABLED) && STATE.enabled) {
        if (!STATE.mode && STATE.customMode) {
            backend->backend->log(AQ_LOG_ERROR, "drm: No mode on enable commit");
            return false;
        }
    }

    if (STATE.adaptiveSync && !connector->canDoVrr) {
        backend->backend->log(AQ_LOG_ERROR, "drm: No Adaptive sync support for output");
        return false;
    }

    if (STATE.presentationMode == AQ_OUTPUT_PRESENTATION_IMMEDIATE && !backend->drmProps.supportsAsyncCommit) {
        backend->backend->log(AQ_LOG_ERROR, "drm: No Immediate presentation support in the backend");
        return false;
    }

    if (COMMITTED & COutputState::eOutputStateProperties::AQ_OUTPUT_STATE_BUFFER && !STATE.buffer) {
        backend->backend->log(AQ_LOG_ERROR, "drm: No buffer committed");
        return false;
    }

    // If we are changing the rendering format, we may need to reconfigure the output (aka modeset)
    // which may result in some glitches
    const bool NEEDS_RECONFIG = COMMITTED &
        (COutputState::eOutputStateProperties::AQ_OUTPUT_STATE_ENABLED | COutputState::eOutputStateProperties::AQ_OUTPUT_STATE_FORMAT |
         COutputState::eOutputStateProperties::AQ_OUTPUT_STATE_MODE);

    const bool BLOCKING = NEEDS_RECONFIG || !(COMMITTED & COutputState::eOutputStateProperties::AQ_OUTPUT_STATE_BUFFER);

    const auto MODE = STATE.mode ? STATE.mode : STATE.customMode;

    uint32_t   flags = 0;

    if (!onlyTest) {
        if (NEEDS_RECONFIG) {
            if (STATE.enabled)
                backend->backend->log(AQ_LOG_DEBUG,
                                      std::format("drm: Modesetting {} with {}x{}@{:.2f}Hz", name, (int)MODE->pixelSize.x, (int)MODE->pixelSize.y, MODE->refreshRate / 1000.F));
            else
                backend->backend->log(AQ_LOG_DEBUG, std::format("drm: Disabling output {}", name));
        }

        if (!BLOCKING && connector->isPageFlipPending) {
            backend->backend->log(AQ_LOG_ERROR, "drm: Cannot commit when a page-flip is awaiting");
            return false;
        }

        if (STATE.enabled)
            flags |= DRM_MODE_PAGE_FLIP_EVENT;
        if (STATE.presentationMode == AQ_OUTPUT_PRESENTATION_IMMEDIATE)
            flags |= DRM_MODE_PAGE_FLIP_ASYNC;
    }

    SDRMConnectorCommitData data;

    if (STATE.buffer) {
        backend->backend->log(AQ_LOG_TRACE, "drm: Committed a buffer, updating state");

        SP<CDRMFB> drmFB;
        auto       buf = STATE.buffer;
        // try to find the buffer in its layer
        if (connector->crtc->primary->back && connector->crtc->primary->back->buffer == buf) {
            backend->backend->log(AQ_LOG_TRACE, "drm: CRTC's back buffer matches committed :D");
            drmFB = connector->crtc->primary->back;
        } else if (connector->crtc->primary->front && connector->crtc->primary->front->buffer == buf) {
            backend->backend->log(AQ_LOG_TRACE, "drm: CRTC's front buffer matches committed");
            drmFB = connector->crtc->primary->front;
        }

        if (!drmFB)
            drmFB = CDRMFB::create(buf, backend);

        if (!drmFB) {
            backend->backend->log(AQ_LOG_ERROR, "drm: Buffer failed to import to KMS");
            return false;
        }

        data.mainFB = drmFB;
    }

    data.blocking = BLOCKING;
    data.modeset  = NEEDS_RECONFIG;
    data.flags    = flags;
    data.test     = onlyTest;
    if (MODE->modeInfo.has_value())
        data.modeInfo = *MODE->modeInfo;
    else
        data.calculateMode(connector);

    bool ok = connector->commitState(data);

    events.commit.emit();

    state->onCommit();

    return ok;
}

SP<IBackendImplementation> Aquamarine::CDRMOutput::getBackend() {
    return backend.lock();
}

bool Aquamarine::CDRMOutput::setCursor(SP<IBuffer> buffer, const Vector2D& hotspot) {
    return false; // FIXME:
}

void Aquamarine::CDRMOutput::moveCursor(const Vector2D& coord) {
    ; // FIXME:
}

void Aquamarine::CDRMOutput::scheduleFrame() {
    if (connector->isPageFlipPending)
        return;

    backend->idleCallbacks.emplace_back([this]() { events.frame.emit(); });
}

Vector2D Aquamarine::CDRMOutput::maxCursorSize() {
    return backend->drmProps.cursorSize;
}

Aquamarine::CDRMOutput::CDRMOutput(const std::string& name_, Hyprutils::Memory::CWeakPointer<CDRMBackend> backend_, SP<SDRMConnector> connector_) :
    backend(backend_), connector(connector_) {
    name = name_;
}

SP<CDRMFB> Aquamarine::CDRMFB::create(SP<IBuffer> buffer_, Hyprutils::Memory::CWeakPointer<CDRMBackend> backend_) {
    auto fb = SP<CDRMFB>(new CDRMFB(buffer_, backend_));

    if (!fb->id)
        return nullptr;

    return fb;
}

Aquamarine::CDRMFB::CDRMFB(SP<IBuffer> buffer_, Hyprutils::Memory::CWeakPointer<CDRMBackend> backend_) : buffer(buffer_), backend(backend_) {
    auto attrs = buffer->dmabuf();
    if (!attrs.success) {
        backend->backend->log(AQ_LOG_ERROR, "drm: Buffer submitted has no dmabuf");
        return;
    }

    if (buffer->attachments.has(AQ_ATTACHMENT_DRM_KMS_UNIMPORTABLE)) {
        backend->backend->log(AQ_LOG_ERROR, "drm: Buffer submitted is unimportable");
        return;
    }

    // TODO: check format

    for (int i = 0; i < attrs.planes; ++i) {
        int ret = drmPrimeFDToHandle(backend->gpu->fd, attrs.fds.at(i), &boHandles.at(i));
        if (ret) {
            backend->backend->log(AQ_LOG_ERROR, "drm: drmPrimeFDToHandle failed");
            drop();
            return;
        }

        backend->backend->log(AQ_LOG_TRACE, std::format("drm: CDRMFB: plane {} has fd {}, got handle {}", i, attrs.fds.at(i), boHandles.at(i)));
    }

    id = submitBuffer();
    if (!id) {
        backend->backend->log(AQ_LOG_ERROR, "drm: Failed to submit a buffer to KMS");
        buffer->attachments.add(makeShared<CDRMBufferUnimportable>());
        drop();
        return;
    }

    backend->backend->log(AQ_LOG_TRACE, std::format("drm: new buffer {}", id));

    // FIXME: wlroots does this, I am unsure why, but if I do, the gpu driver will kill us.
    // closeHandles();
}

Aquamarine::CDRMFB::~CDRMFB() {
    drop();
}

void Aquamarine::CDRMFB::closeHandles() {
    if (handlesClosed)
        return;

    handlesClosed = true;

    for (auto& h : boHandles) {
        if (h == 0)
            continue;

        if (drmCloseBufferHandle(backend->gpu->fd, h))
            backend->backend->log(AQ_LOG_ERROR, "drm: drmCloseBufferHandle failed");
        h = 0;
    }
}

void Aquamarine::CDRMFB::drop() {
    if (dropped)
        return;

    dropped = true;

    if (!id)
        return;

    backend->backend->log(AQ_LOG_TRACE, std::format("drm: dropping buffer {}", id));

    int ret = drmModeCloseFB(backend->gpu->fd, id);
    if (ret == -EINVAL)
        ret = drmModeRmFB(backend->gpu->fd, id);

    if (ret)
        backend->backend->log(AQ_LOG_ERROR, std::format("drm: Failed to close a buffer: {}", strerror(-ret)));
}

uint32_t Aquamarine::CDRMFB::submitBuffer() {
    auto                    attrs = buffer->dmabuf();
    uint32_t                newID = 0;
    std::array<uint64_t, 4> mods  = {0};
    for (size_t i = 0; i < attrs.planes; ++i) {
        mods.at(i) = attrs.modifier;
    }

    if (backend->drmProps.supportsAddFb2Modifiers && attrs.modifier != DRM_FORMAT_MOD_INVALID) {
        backend->backend->log(AQ_LOG_TRACE,
                              std::format("drm: Using drmModeAddFB2WithModifiers to import buffer into KMS: Size {} with format {} and mod {}", attrs.size,
                                          fourccToName(attrs.format), attrs.modifier));
        if (drmModeAddFB2WithModifiers(backend->gpu->fd, attrs.size.x, attrs.size.y, attrs.format, boHandles.data(), attrs.strides.data(), attrs.offsets.data(), mods.data(),
                                       &newID, DRM_MODE_FB_MODIFIERS)) {
            backend->backend->log(AQ_LOG_ERROR, "drm: Failed to submit a buffer with AddFB2");
            return 0;
        }
    } else {
        if (attrs.modifier != DRM_FORMAT_MOD_INVALID && attrs.modifier != DRM_FORMAT_MOD_LINEAR) {
            backend->backend->log(AQ_LOG_ERROR, "drm: drmModeAddFB2WithModifiers unsupported and buffer has explicit modifiers");
            return 0;
        }

        backend->backend->log(
            AQ_LOG_TRACE,
            std::format("drm: Using drmModeAddFB2 to import buffer into KMS: Size {} with format {} and mod {}", attrs.size, fourccToName(attrs.format), attrs.modifier));

        if (drmModeAddFB2(backend->gpu->fd, attrs.size.x, attrs.size.y, attrs.format, boHandles.data(), attrs.strides.data(), attrs.offsets.data(), &newID, 0)) {
            backend->backend->log(AQ_LOG_ERROR, "drm: drmModeAddFB2 failed");
            return 0;
        }
    }

    return newID;
}

void Aquamarine::SDRMConnectorCommitData::calculateMode(Hyprutils::Memory::CSharedPointer<SDRMConnector> connector) {
    const auto&    STATE = connector->output->state->state();
    const auto     MODE  = STATE.mode ? STATE.mode : STATE.customMode;

    di_cvt_options options = {
        .red_blank_ver = DI_CVT_REDUCED_BLANKING_NONE,
        .h_pixels      = (int)MODE->pixelSize.x,
        .v_lines       = (int)MODE->pixelSize.y,
        .ip_freq_rqd   = MODE->refreshRate ? MODE->refreshRate / 1000.0 : 60.0,
    };
    di_cvt_timing timing;

    di_cvt_compute(&timing, &options);

    uint16_t hsync_start = (int)MODE->pixelSize.y + timing.h_front_porch;
    uint16_t vsync_start = timing.v_lines_rnd + timing.v_front_porch;
    uint16_t hsync_end   = hsync_start + timing.h_sync;
    uint16_t vsync_end   = vsync_start + timing.v_sync;

    modeInfo = (drmModeModeInfo){
        .clock       = (uint32_t)std::round(timing.act_pixel_freq * 1000),
        .hdisplay    = (uint16_t)MODE->pixelSize.y,
        .hsync_start = hsync_start,
        .hsync_end   = hsync_end,
        .htotal      = (uint16_t)(hsync_end + timing.h_back_porch),
        .vdisplay    = (uint16_t)timing.v_lines_rnd,
        .vsync_start = vsync_start,
        .vsync_end   = vsync_end,
        .vtotal      = (uint16_t)(vsync_end + timing.v_back_porch),
        .vrefresh    = (uint32_t)std::round(timing.act_frame_rate),
        .flags       = DRM_MODE_FLAG_NHSYNC | DRM_MODE_FLAG_PVSYNC,
    };
    snprintf(modeInfo.name, sizeof(modeInfo.name), "%dx%d", (int)MODE->pixelSize.x, (int)MODE->pixelSize.y);
}