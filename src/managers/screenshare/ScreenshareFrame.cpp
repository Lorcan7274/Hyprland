#include "ScreenshareManager.hpp"
#include "WorkspaceCaptureSource.hpp"
#include "../../pointer/PointerManager.hpp"
#include "../SeatManager.hpp"
#include "../SessionLockManager.hpp"
#include "../permissions/DynamicPermissionManager.hpp"
#include "../../protocols/ColorManagement.hpp"
#include "../../Compositor.hpp"
#include "../../render/Renderer.hpp"
#include "../../render/SceneResources.hpp"
#include "../../render/scene/SceneSelection.hpp"
#include "../../render/OpenGL.hpp"
#include "../../output/Monitor.hpp"
#include "../../state/MonitorState.hpp"
#include "../../workspace/HLWorkspace.hpp"
#include "../../render/scene/MonitorScene.hpp"
#include "../../desktop/view/LayerSurface.hpp"
#include "../../desktop/state/FadingOutState.hpp"
#include "../../desktop/state/LayerState.hpp"
#include "../../desktop/state/WindowState.hpp"
#include "../../desktop/view/window/Window.hpp"
#include "../../desktop/view/window/WindowPresentation.hpp"
#include "../../desktop/state/FocusState.hpp"
#include "../../render/pass/ClearPassElement.hpp"
#include "helpers/cm/ColorManagement.hpp"
#include <hyprutils/math/Region.hpp>
#include <hyprgraphics/egl/Egl.hpp>
#include <hyprland-workspace-image-capture-source-v1.hpp>

using namespace Hyprgraphics::Egl;
using namespace Screenshare;
using namespace Desktop::View;

// superset of what the scene would draw: a false positive only costs a re-render
static bool hasNoScreenShareViews(PHLMONITOR monitor) {
    for (auto const& l : Desktop::layerState()->layers()) {
        if (l->m_monitor == monitor && l->mapped() && l->m_ruleApplicator->noScreenShare().valueOrDefault())
            return true;
    }

    for (auto const& w : Desktop::windowState()->windows()) {
        if (w->m_ruleApplicator->noScreenShare().valueOrDefault() && g_pHyprRenderer->shouldRenderWindow(w, monitor))
            return true;
    }

    for (auto const& f : Desktop::fadingOutState()->fadeouts()) {
        if (f && f->monitor() == monitor && f->source().noScreenShare)
            return true;
    }

    return false;
}

CScreenshareFrame::CScreenshareFrame(WP<CScreenshareSession> session, bool overlayCursor, bool isFirst) :
    m_session(session), m_bufferSize(m_session->bufferSize()), m_overlayCursor(overlayCursor), m_isFirst(isFirst) {
    ;
}

CScreenshareFrame::~CScreenshareFrame() {
    if (m_failed || !m_shared)
        return;

    if (!m_copied && m_callback) {
        FScreenshareCallback cb;
        std::swap(cb, m_callback);
        cb(RESULT_NOT_COPIED);
    }
}

bool CScreenshareFrame::done() const {
    if (m_session.expired() || m_session->m_stopped)
        return true;

    if (m_session->m_type == SHARE_NONE || m_bufferSize == Vector2D(0, 0))
        return true;

    if (m_failed || m_copied)
        return true;

    if (m_session->m_type == SHARE_MONITOR && !m_session->monitor())
        return true;

    if (m_session->m_type == SHARE_REGION && !m_session->monitor())
        return true;

    if (m_session->m_type == SHARE_WORKSPACE && !m_session->monitor())
        return true;

    if (m_session->m_type == SHARE_WINDOW && (!m_session->monitor() || !validMapped(m_session->m_window)))
        return true;

    if (!m_shared)
        return false;

    if (!m_buffer || !m_buffer->m_resource || !m_buffer->m_resource->good())
        return true;

    if (!m_callback)
        return true;

    return false;
}

eScreenshareError CScreenshareFrame::share(SP<IHLBuffer> buffer, const CRegion& clientDamage, FScreenshareCallback callback) {
    if UNLIKELY (done())
        return ERROR_STOPPED;

    if UNLIKELY (!m_session->monitor() || !State::monitorState()->contains(m_session->monitor())) {
        LOG(Log::ERR, "Client requested sharing of a monitor that is gone");
        m_failed = true;
        return ERROR_STOPPED;
    }

    if UNLIKELY (m_session->m_type == SHARE_WINDOW && !validMapped(m_session->m_window)) {
        LOG(Log::ERR, "Client requested sharing of window that is gone or not shareable!");
        m_failed = true;
        return ERROR_STOPPED;
    }

    if UNLIKELY (!buffer || !buffer->m_resource || !buffer->m_resource->good()) {
        LOG(Log::ERR, "Client requested sharing to an invalid buffer");
        return ERROR_NO_BUFFER;
    }

    if UNLIKELY (m_bufferSize != m_session->bufferSize()) {
        LOG(Log::ERR, "Client requested sharing with stale buffer constraints");
        return ERROR_BUFFER_SIZE;
    }

    if UNLIKELY (buffer->size != m_bufferSize) {
        LOG(Log::ERR, "Client requested sharing to an invalid buffer size");
        return ERROR_BUFFER_SIZE;
    }

    uint32_t bufFormat;
    if (buffer->dmabuf().success)
        bufFormat = buffer->dmabuf().format;
    else if (buffer->shm().success)
        bufFormat = buffer->shm().format;
    else {
        LOG(Log::ERR, "Client requested sharing to an invalid buffer");
        return ERROR_NO_BUFFER;
    }

    if (std::ranges::count_if(m_session->allowedFormats(), [&](const DRMFormat& format) { return format == bufFormat; }) == 0) {
        LOG(Log::ERR, "Invalid format {} in {:x}", bufFormat, (uintptr_t)this);
        return ERROR_BUFFER_FORMAT;
    }

    m_buffer   = buffer;
    m_callback = callback;
    m_shared   = true;

    if (m_session->m_type == SHARE_MONITOR || m_session->m_type == SHARE_REGION) {
        const auto PMONITOR = m_session->monitor();
        if (PMONITOR)
            PMONITOR->addDamage(PMONITOR->resources()->pendingMirrorFBDamage());
    }

    // schedule a frame so that when a screenshare starts it isn't black until the output is updated
    if (m_isFirst || m_session->m_type == SHARE_WORKSPACE) {
        const auto PMONITOR = m_session->monitor();
        if (PMONITOR)
            PMONITOR->scheduleFrame(Aquamarine::IOutput::AQ_SCHEDULE_NEEDS_FRAME);
        g_pHyprRenderer->damageMonitor(PMONITOR);
    }

    // TODO: add a damage ring for output damage since last shared frame
    CRegion frameDamage = CRegion(0, 0, m_bufferSize.x, m_bufferSize.y);

    // copy everything on the first frame
    if (m_isFirst)
        m_damage = CRegion(0, 0, m_bufferSize.x, m_bufferSize.y);
    else
        m_damage = frameDamage.add(clientDamage);

    m_damage.intersect(0, 0, m_bufferSize.x, m_bufferSize.y);

    return ERROR_NONE;
}

void CScreenshareFrame::copy() {
    if (g_pHyprRenderer->context().active() || done() || m_copyInFlight)
        return;

    // tell client to send presented timestamp
    // TODO: is this right? this is right after we commit to aq, not when page flip happens..
    m_callback(RESULT_TIMESTAMP);

    if (g_pHyprRenderer->context().active())
        return;

    // store a snapshot before the permission popup so we don't break screenshots
    const auto PERM        = g_pDynamicPermissionManager->clientPermissionMode(m_session->m_client, PERMISSION_TYPE_SCREENCOPY);
    const bool HAS_TEMP_FB = m_session->m_tempFB && m_session->m_tempFB->isAllocated();

    if (PERM != PERMISSION_RULE_ALLOW_MODE_DENY && !HAS_TEMP_FB && !renderFilteredScene()) {
        if (PERM == PERMISSION_RULE_ALLOW_MODE_PENDING)
            return; // retry on the next commit

        m_failed = true;
        m_callback(RESULT_NOT_COPIED);
        return;
    }

    if (PERM == PERMISSION_RULE_ALLOW_MODE_PENDING) {
        if (m_session->m_type != SHARE_WORKSPACE && !HAS_TEMP_FB)
            storeTempFB();

        // don't copy a frame while allow is pending because screenshot tools will only take the first frame we give, which is empty
        return;
    }

    if (m_session->m_type == SHARE_WORKSPACE && (PERM == PERMISSION_RULE_ALLOW_MODE_DENY || m_bufferSize != m_session->bufferSize())) {
        m_failed = true;
        m_callback(RESULT_NOT_COPIED);
        return;
    }

    if (m_buffer->shm().success)
        m_failed = !copyShm();
    else if (m_buffer->dmabuf().success)
        m_failed = !copyDmabuf();

    if (!m_failed) {
        // screensharing has started again
        m_session->screenshareEvents(true);
        m_session->m_shareStopTimer->updateTimeout(std::chrono::milliseconds(500)); // check in half second
    } else
        m_callback(RESULT_NOT_COPIED);
}

void CScreenshareFrame::renderMonitor(Render::CRenderContext& ctx) {
    if ((m_session->m_type != SHARE_MONITOR && m_session->m_type != SHARE_REGION) || done())
        return;

    const auto PMONITOR = m_session->monitor();

    // the mirror FB already contains noscreenshare views, so those frames are sourced from a re-render without them
    auto TEXTURE = m_useFilteredScene && m_session->m_filteredSceneFB ? m_session->m_filteredSceneFB->getTexture() : ctx.m_data.pMonitor->resources()->getMirrorTexture();
    if (!TEXTURE) {
        LOG(Log::ERR, "Invalid source texture");
        return;
    }

    if (!TEXTURE->m_imageDescription)
        LOG(Log::ERR, "CM: FIXME no source image description for screenshare");

    if (!ctx.m_data.currentFB->imageDescription())
        LOG(Log::ERR, "CM: FIXME no target image description for screenshare");

    if (TEXTURE->m_imageDescription && ctx.m_data.currentFB->imageDescription())
        LOG(Log::TRACE, "CM: screenshot renderMonitor {} -> {}", TEXTURE->m_imageDescription->value(), ctx.m_data.currentFB->imageDescription()->value());

    const bool IS_CM_AWARE = PROTO::colorManagement && PROTO::colorManagement->isClientCMAware(m_session->m_client);
    ctx.m_data.fbSize      = m_bufferSize;
    g_pHyprRenderer->setProjectionType(ctx, Render::RPT_EXPORT);
    ctx.m_data.transformDamage = false;
    ctx.m_data.noSimplify      = true;
    g_pHyprRenderer->setViewport(0, 0, m_bufferSize.x, m_bufferSize.y);

    // render monitor texture
    CBox       monbox = CBox{{}, PMONITOR->m_transformedSize}.translate(-m_session->m_captureBox.pos());

    const auto OLD                 = ctx.m_data.renderModif.enabled;
    ctx.m_data.renderModif.enabled = false;
    g_pHyprRenderer->startRenderPass(ctx);
    g_pHyprRenderer->draw(ctx,
                          CTexPassElement::SRenderData{
                              .tex          = TEXTURE,
                              .box          = monbox,
                              .cmBackToSRGB = !IS_CM_AWARE,
                          },
                          {0, 0, m_bufferSize.x, m_bufferSize.y});
    ctx.m_data.renderModif.enabled = OLD;

    if (m_overlayCursor) {
        CRegion  fakeDamage = {0, 0, m_bufferSize.x, m_bufferSize.y};
        Vector2D cursorPos  = Pointer::mgr()->untransformedPosition() - PMONITOR->m_position - m_session->m_captureBox.pos() / PMONITOR->m_scale;
        Pointer::mgr()->renderSoftwareCursorsFor(ctx, PMONITOR, Time::steadyNow(), fakeDamage, cursorPos, true);
    }
}

bool CScreenshareFrame::renderFilteredScene() {
    m_useFilteredScene = false;

    if (m_session->m_type != SHARE_MONITOR && m_session->m_type != SHARE_REGION)
        return true;

    const auto PMONITOR = m_session->monitor();
    if (!PMONITOR)
        return false;

    if (!hasNoScreenShareViews(PMONITOR)) {
        m_session->m_sceneResources.reset();
        m_session->m_filteredSceneFB.reset();
        return true;
    }

    if (g_pHyprRenderer->context().active())
        return false;

    auto& FB = m_session->m_filteredSceneFB;
    if (!FB)
        FB = g_pHyprRenderer->createFB("screenshare filtered scene");

    if (!FB || !FB->alloc(PMONITOR->m_transformedSize.x, PMONITOR->m_transformedSize.y, PMONITOR->useFP16() ? DRM_FORMAT_ABGR16161616F : DRM_FORMAT_ABGR8888)) {
        LOG(Log::ERR, "Can't copy: failed to allocate the filtered scene fb");
        return false;
    }
    FB->setImageDescription(PMONITOR->workBufferImageDescription());

    // isolated resources keep this render's blur state away from the monitor's own
    if (!m_session->m_sceneResources)
        m_session->m_sceneResources = makeShared<Render::CSceneResources>(g_pHyprRenderer->createFB("screenshare filtered scene blur"));

    CRegion                fakeDamage = {0, 0, PMONITOR->m_transformedSize.x, PMONITOR->m_transformedSize.y};
    Render::SRenderOptions options;
    options.sceneResources = m_session->m_sceneResources;

    if (!g_pHyprRenderer->beginRender(PMONITOR, fakeDamage, Render::RENDER_MODE_FULL_FAKE, {}, FB, false, options)) {
        LOG(Log::ERR, "Can't copy: failed to begin rendering the filtered scene");
        return false;
    }
    bool                      finishing = false;
    const Render::CScopeGuard cleanup([&] {
        if (!finishing)
            g_pHyprRenderer->abortRender();
    });

    auto&                     ctx = g_pHyprRenderer->context();
    ctx.m_renderingCapture        = true;
    ctx.m_blockSurfaceFeedback    = true; // the monitor's own frame reports presentation

    g_pHyprRenderer->draw(ctx, CClearPassElement::SClearData{Colors::BLACK});
    g_pHyprRenderer->startRenderPass(ctx);

    Render::CMonitorScene scene(PMONITOR);
    scene.draw(ctx, Time::steadyNow());

    finishing = true;
    g_pHyprRenderer->endRender();

    m_useFilteredScene = true;
    return true;
}

void CScreenshareFrame::renderWindow(Render::CRenderContext& ctx) {
    if (m_session->m_type != SHARE_WINDOW || done())
        return;

    const auto PWINDOW  = m_session->m_window.lock();
    const auto PMONITOR = m_session->monitor();

    const auto NOW = Time::steadyNow();

    // TODO: implement a monitor independent render mode to buffer that does this in CHyprRenderer::begin() or something like that
    ctx.m_data.fbSize = m_bufferSize;
    g_pHyprRenderer->setProjectionType(ctx, Render::RPT_EXPORT);
    ctx.m_data.transformDamage = false;
    g_pHyprRenderer->setViewport(0, 0, m_bufferSize.x, m_bufferSize.y);

    ctx.m_blockSurfaceFeedback = g_pHyprRenderer->shouldRenderWindow(PWINDOW); // block the feedback to avoid spamming the surface if it's visible
    g_pHyprRenderer->renderWindow(ctx, PWINDOW, PMONITOR, PWINDOW->presentation().renderPresentation(nullptr), NOW, false, Render::RENDER_PASS_ALL, true, true);
    ctx.m_blockSurfaceFeedback = false;

    if (!m_overlayCursor)
        return;

    auto pointerSurfaceResource = g_pSeatManager->m_state.pointerFocus.lock();

    if (!pointerSurfaceResource)
        return;

    auto pointerSurface = Desktop::View::CWLSurface::fromResource(pointerSurfaceResource);
    if (!pointerSurface)
        return;

    auto box = pointerSurface->getSurfaceBoxGlobal();
    if (!box.has_value() || box->intersection(m_session->m_window->getFullWindowBoundingBox()).empty())
        return;

    if (Desktop::focusState()->window() != m_session->m_window)
        return;

    CRegion fakeDamage = {0, 0, INT16_MAX, INT16_MAX};
    Pointer::mgr()->renderSoftwareCursorsFor(ctx, PMONITOR->m_self.lock(), NOW, fakeDamage,
                                             Pointer::mgr()->untransformedPosition() - PWINDOW->position(Desktop::View::IGeometric::GEOMETRIC_CURRENT), true, true);
}

void CScreenshareFrame::renderWorkspace(Render::CRenderContext& ctx) {
    const auto WORKSPACE = m_session->m_workspace->workspace();
    const auto MONITOR   = ctx.m_data.pMonitor.lock();
    if (!WORKSPACE || WORKSPACE->m_monitor != MONITOR || g_pSessionLockManager->isSessionLocked())
        return;

    const auto MODE = m_session->m_workspace->mode() == HYPRLAND_WORKSPACE_IMAGE_CAPTURE_SOURCE_MANAGER_V1_CAPTURE_MODE_WINDOWS_ONLY ? Render::eSceneMode::WORKSPACE_WINDOWS :
                                                                                                                                       Render::eSceneMode::WORKSPACE_WITH_SHELL;
    const auto NOW  = Time::steadyNow();
    g_pHyprRenderer->startRenderPass(ctx);
    g_pHyprRenderer->renderWorkspace(ctx, WORKSPACE, NOW, MODE);

    if (!m_overlayCursor || (MONITOR->m_activeWorkspace != WORKSPACE && MONITOR->m_activeSpecialWorkspace != WORKSPACE))
        return;

    CRegion damage{0, 0, m_bufferSize.x, m_bufferSize.y};
    Pointer::mgr()->renderSoftwareCursorsFor(ctx, MONITOR, NOW, damage, Pointer::mgr()->untransformedPosition() - MONITOR->m_position, true, true);
}

void CScreenshareFrame::render(Render::CRenderContext& ctx) {
    const auto PERM = g_pDynamicPermissionManager->clientPermissionMode(m_session->m_client, PERMISSION_TYPE_SCREENCOPY);

    // Removed sources keep their last export size even if the output changes size.
    // Clear all scene scratch, not just the frozen export rectangle.
    const auto CLEAR_SIZE  = m_session->m_type == SHARE_WORKSPACE ? ctx.m_data.currentFB->m_size : m_bufferSize;
    CRegion    frameRegion = {0, 0, CLEAR_SIZE.x, CLEAR_SIZE.y};
    g_pHyprRenderer->draw(ctx, CClearPassElement::SClearData{{0, 0, 0, 0}}, frameRegion);

    if (m_session->m_type == SHARE_WORKSPACE) {
        if (PERM == PERMISSION_RULE_ALLOW_MODE_ALLOW)
            renderWorkspace(ctx);
        return;
    }

    if (PERM == PERMISSION_RULE_ALLOW_MODE_PENDING)
        return;

    bool windowShareDenied = m_session->m_type == SHARE_WINDOW && m_session->m_window->m_ruleApplicator && m_session->m_window->m_ruleApplicator->noScreenShare().valueOrDefault();
    g_pHyprRenderer->startRenderPass(ctx);
    if (PERM == PERMISSION_RULE_ALLOW_MODE_DENY || windowShareDenied) {
        CBox texbox = CBox{m_bufferSize / 2.F, g_pHyprRenderer->m_screencopyDeniedTexture->m_size}.translate(-g_pHyprRenderer->m_screencopyDeniedTexture->m_size / 2.F);
        g_pHyprRenderer->draw(ctx, CTexPassElement::SRenderData{.tex = g_pHyprRenderer->m_screencopyDeniedTexture, .box = texbox}, texbox);
        return;
    }

    if (m_session->m_tempFB && m_session->m_tempFB->isAllocated()) {
        CBox texbox = {{}, m_bufferSize};
        g_pHyprRenderer->draw(ctx, CTexPassElement::SRenderData{.tex = m_session->m_tempFB->getTexture(), .box = texbox}, texbox);
        m_session->m_tempFB->release();
        return;
    }

    switch (m_session->m_type) {
        case SHARE_REGION: // TODO: could this be better? this is how screencopy works
        case SHARE_MONITOR: renderMonitor(ctx); break;
        case SHARE_WINDOW: renderWindow(ctx); break;
        case SHARE_NONE:
        default: return;
    }
}

bool CScreenshareFrame::beginCopy(SP<Render::IFramebuffer> framebuffer) {
    const bool             WORKSPACE = m_session->m_type == SHARE_WORKSPACE;
    Render::SRenderOptions options;
    if (WORKSPACE) {
        if (!m_session->m_sceneResources)
            m_session->m_sceneResources = makeShared<Render::CSceneResources>(g_pHyprRenderer->createFB("workspace capture blur"),
                                                                              Render::SSceneBufferDescription{
                                                                                  .format           = DRM_FORMAT_ARGB8888,
                                                                                  .imageDescription = NColorManagement::DEFAULT_SRGB_IMAGE_DESCRIPTION,
                                                                              });
        options.sceneResources = m_session->m_sceneResources;
    }

    // beginRender may replace scene damage. Preserve the client's export damage.
    CRegion damage = m_damage;
    if (!g_pHyprRenderer->beginRender(m_session->monitor(), damage, framebuffer ? Render::RENDER_MODE_FULL_FAKE : Render::RENDER_MODE_TO_BUFFER, framebuffer ? nullptr : m_buffer,
                                      framebuffer, !WORKSPACE, options))
        return false;

    auto& ctx = g_pHyprRenderer->context();
    ctx.m_data.outFB->setImageDescription(NColorManagement::DEFAULT_SRGB_IMAGE_DESCRIPTION);
    ctx.m_data.blockScreenShader = true;
    ctx.m_renderingCapture       = WORKSPACE;
    ctx.m_blockSurfaceFeedback   = WORKSPACE;
    return true;
}

bool CScreenshareFrame::copyDmabuf() {
    if (g_pHyprRenderer->context().active() || done())
        return false;

    if (!beginCopy()) {
        LOG(Log::ERR, "Can't copy: failed to begin rendering to dma frame");
        return false;
    }
    bool                      finishing = false;
    const Render::CScopeGuard cleanup([&] {
        if (!finishing)
            g_pHyprRenderer->abortRender();
    });
    auto&                     ctx = g_pHyprRenderer->context();
    render(ctx);

    m_copyInFlight = true;

    finishing = true;
    g_pHyprRenderer->endRender([self = m_self]() {
        if (!self || self.expired())
            return;

        self->m_copyInFlight = false;

        if (self->m_copied)
            return;

        LOG(Log::TRACE, "Copied frame via dma");
        self->m_callback(RESULT_COPIED);
        self->m_copied = true;
    });

    return true;
}

bool CScreenshareFrame::copyShm() {
    if (g_pHyprRenderer->context().active() || done())
        return false;

    auto       shm = m_buffer->shm();

    const auto PFORMAT = getPixelFormatFromDRM(shm.format);
    if (!PFORMAT) {
        LOG(Log::ERR, "Can't copy: failed to find a pixel format");
        return false;
    }

    auto outFB = g_pHyprRenderer->createFB();
    if (!outFB || !outFB->alloc(m_bufferSize.x, m_bufferSize.y, shm.format))
        return false;
    outFB->setImageDescription(NColorManagement::DEFAULT_SRGB_IMAGE_DESCRIPTION);

    if (!beginCopy(outFB)) {
        LOG(Log::ERR, "Can't copy: failed to begin rendering");
        return false;
    }
    bool                      finishing = false;
    const Render::CScopeGuard cleanup([&] {
        if (!finishing)
            g_pHyprRenderer->abortRender();
    });

    auto&                     ctx = g_pHyprRenderer->context();
    render(ctx);

    finishing = true;
    g_pHyprRenderer->endRender();

    bool readSucceeded = true;
    m_damage.forEachRect([&](const auto& rect) {
        if (!readSucceeded)
            return;

        int width  = rect.x2 - rect.x1;
        int height = rect.y2 - rect.y1;
        if (!outFB->readPixels(m_buffer, rect.x1, rect.y1, width, height))
            readSucceeded = false;
    });

    if (!readSucceeded) {
        LOG(Log::ERR, "Can't copy: failed to read pixels to shm");
        return false;
    }

    if (!m_copied) {
        LOG(Log::TRACE, "Copied frame via shm");
        m_callback(RESULT_COPIED);
        m_copied = true;
    }

    return true;
}

void CScreenshareFrame::storeTempFB() {
    if (g_pHyprRenderer->context().active())
        return;

    if (!m_session->m_tempFB)
        m_session->m_tempFB = g_pHyprRenderer->createFB();
    m_session->m_tempFB->alloc(m_bufferSize.x, m_bufferSize.y);
    m_session->m_tempFB->setImageDescription(NColorManagement::DEFAULT_SRGB_IMAGE_DESCRIPTION);

    CRegion fakeDamage = {0, 0, m_bufferSize.x, m_bufferSize.y};

    if (!g_pHyprRenderer->beginFullFakeRender(m_session->monitor(), fakeDamage, m_session->m_tempFB)) {
        LOG(Log::ERR, "Can't copy: failed to begin rendering to temp fb");
        return;
    }
    bool                      finishing = false;
    const Render::CScopeGuard cleanup([&] {
        if (!finishing)
            g_pHyprRenderer->abortRender();
    });

    auto&                     ctx = g_pHyprRenderer->context();
    switch (m_session->m_type) {
        case SHARE_REGION: // TODO: could this be better? this is how screencopy works
        case SHARE_MONITOR: renderMonitor(ctx); break;
        case SHARE_WINDOW: renderWindow(ctx); break;
        case SHARE_NONE:
        default: return;
    }

    finishing = true;
    g_pHyprRenderer->endRender();
}

Vector2D CScreenshareFrame::bufferSize() const {
    return m_bufferSize;
}

wl_output_transform CScreenshareFrame::transform() const {
    return WL_OUTPUT_TRANSFORM_NORMAL;
}

const CRegion& CScreenshareFrame::damage() const {
    return m_damage;
}
