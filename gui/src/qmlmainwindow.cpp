#include "qmlmainwindow.h"
#include "qmlbackend.h"
#include "chiaki/log.h"
#include "streamsession.h"

#include <xcb/xcb.h>
#include <vulkan/vulkan.h>
#include <vulkan/vulkan_xcb.h>
#include <vulkan/vulkan_wayland.h>
#include <qpa/qplatformnativeinterface.h>

#define PL_LIBAV_IMPLEMENTATION 0
#include <libplacebo/utils/libav.h>

#include <QDebug>
#include <QThread>
#include <QShortcut>
#include <QStandardPaths>
#include <QGuiApplication>
#include <QQmlComponent>
#include <QQmlEngine>
#include <QQuickRenderControl>
#include <QQuickItem>
#include <QOpenGLContext>
#include <QOpenGLFunctions>
#include <QOpenGLExtraFunctions>

Q_LOGGING_CATEGORY(chiakiGui, "chiaki.gui", QtInfoMsg);

static ChiakiLog *chiaki_log_ctx = nullptr;
static QtMessageHandler qt_msg_handler = nullptr;

static void placebo_log_cb(void *user, pl_log_level level, const char *msg)
{
    ChiakiLogLevel chiaki_level;
    switch (level) {
    case PL_LOG_NONE:
    case PL_LOG_TRACE:
    case PL_LOG_DEBUG:
        qCDebug(chiakiGui).noquote() << "[libplacebo]" << msg;
        break;
    case PL_LOG_INFO:
        qCInfo(chiakiGui).noquote() << "[libplacebo]" << msg;
        break;
    case PL_LOG_WARN:
        qCWarning(chiakiGui).noquote() << "[libplacebo]" << msg;
        break;
    case PL_LOG_ERR:
    case PL_LOG_FATAL:
        qCCritical(chiakiGui).noquote() << "[libplacebo]" << msg;
        break;
    }
}

static void msg_handler(QtMsgType type, const QMessageLogContext &context, const QString &msg)
{
    if (!chiaki_log_ctx) {
        qt_msg_handler(type, context, msg);
        return;
    }
    ChiakiLogLevel chiaki_level;
    switch (type) {
    case QtDebugMsg:
        chiaki_level = CHIAKI_LOG_DEBUG;
        break;
    case QtInfoMsg:
        chiaki_level = CHIAKI_LOG_INFO;
        break;
    case QtWarningMsg:
        chiaki_level = CHIAKI_LOG_WARNING;
        break;
    case QtCriticalMsg:
        chiaki_level = CHIAKI_LOG_ERROR;
        break;
    case QtFatalMsg:
        chiaki_level = CHIAKI_LOG_ERROR;
        break;
    }
    chiaki_log(chiaki_log_ctx, chiaki_level, "%s", qPrintable(msg));
}

static const char *shader_cache_path()
{
    static QString path = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation) + "/pl_shader.cache";
    return qPrintable(path);
}

QmlMainWindow::QmlMainWindow(Settings *settings)
    : QWindow()
{
    init(settings);
}

QmlMainWindow::QmlMainWindow(const StreamSessionConnectInfo &connect_info)
    : QWindow()
{
    init(connect_info.settings);
    backend->createSession(connect_info);
}

QmlMainWindow::~QmlMainWindow()
{
    Q_ASSERT(closing && !placebo_swapchain);

    QMetaObject::invokeMethod(quick_render, [this]() {
        quick_render->invalidate();
        delete gl_fbo;
        gl_context->doneCurrent();
        gl_context->moveToThread(QGuiApplication::instance()->thread());
    }, Qt::BlockingQueuedConnection);

    render_thread->quit();
    render_thread->wait();
    delete render_thread->parent();

    delete quick_render;
    delete quick_item;
    delete quick_window;
    delete qml_engine;
    delete gl_surface;
    delete gl_context;

    for (int i = 0; i < 4; i++)
        if (placebo_tex[i])
            pl_tex_destroy(placebo_vulkan->gpu, &placebo_tex[i]);

    FILE *file = fopen(shader_cache_path(), "wb");
    if (file) {
        pl_cache_save_file(placebo_cache, file);
        fclose(file);
    }
    pl_cache_destroy(&placebo_cache);
    pl_renderer_destroy(&placebo_renderer);
    pl_vulkan_destroy(&placebo_vulkan);
    pl_vk_inst_destroy(&placebo_vk_inst);
    pl_log_destroy(&placebo_log);
}

bool QmlMainWindow::hasVideo() const
{
    return has_video;
}

int QmlMainWindow::corruptedFrames() const
{
    return corrupted_frames;
}

bool QmlMainWindow::keepVideo() const
{
    return keep_video;
}

void QmlMainWindow::setKeepVideo(bool keep)
{
    keep_video = keep;
    emit keepVideoChanged();
}

bool QmlMainWindow::grabInput() const
{
    return grab_input;
}

void QmlMainWindow::setGrabInput(bool grab)
{
    grab_input = grab;
    if (session)
        session->BlockInput(grab_input);
    if (grab_input)
        setCursor(Qt::ArrowCursor);
    else
        setCursor(Qt::BlankCursor);
    emit grabInputChanged();
}

QmlMainWindow::VideoMode QmlMainWindow::videoMode() const
{
    return video_mode;
}

void QmlMainWindow::setVideoMode(VideoMode mode)
{
    video_mode = mode;
    emit videoModeChanged();
}

QmlMainWindow::VideoPreset QmlMainWindow::videoPreset() const
{
    return video_preset;
}

void QmlMainWindow::setVideoPreset(VideoPreset preset)
{
    video_preset = preset;
    emit videoPresetChanged();
}

void QmlMainWindow::show()
{
    QQmlComponent component(qml_engine, QUrl(QStringLiteral("qrc:/Main.qml")));
    if (!component.isReady()) {
        qCCritical(chiakiGui) << "Component not ready\n" << component.errors();
        return;
    }

    QVariantMap props;
    props[QStringLiteral("parent")] = QVariant::fromValue(quick_window->contentItem());
    quick_item = qobject_cast<QQuickItem*>(component.createWithInitialProperties(props));
    if (!quick_item) {
        qCCritical(chiakiGui) << "Failed to create root item\n" << component.errors();
        return;
    }

    resize(800, 600);

    if (qEnvironmentVariable("XDG_CURRENT_DESKTOP") == "gamescope")
        showFullScreen();
    else
        showNormal();
}

void QmlMainWindow::presentFrame(AVFrame *frame)
{
    int corrupted_now = corrupted_frames;

    frame_mutex.lock();
    if (frame->decode_error_flags) {
        corrupted_now++;
        qCDebug(chiakiGui) << "Dropping decode error frame";
        av_frame_free(&frame);
    } else if (next_frame) {
        qCDebug(chiakiGui) << "Dropping rendering frame";
        av_frame_free(&next_frame);
    }
    if (frame)
        next_frame = frame;
    frame_mutex.unlock();

    if (corrupted_now == corrupted_frames)
        corrupted_now = 0;

    if (corrupted_now != corrupted_frames) {
        corrupted_frames = corrupted_now;
        emit corruptedFramesChanged();
    }

    if (!has_video) {
        has_video = true;
        if (!grab_input)
            setCursor(Qt::BlankCursor);
        emit hasVideoChanged();
    }

    update();
}

void QmlMainWindow::init(Settings *settings)
{
    setSurfaceType(QWindow::VulkanSurface);

    qt_msg_handler = qInstallMessageHandler(msg_handler);

    const char *vk_exts[] = {
        nullptr,
        VK_KHR_SURFACE_EXTENSION_NAME,
    };

    QString platformName = QGuiApplication::platformName();
    if (platformName.startsWith("wayland")) {
        vk_exts[0] = VK_KHR_WAYLAND_SURFACE_EXTENSION_NAME;
    } else if (platformName.startsWith("xcb")) {
        vk_exts[0] = VK_KHR_XCB_SURFACE_EXTENSION_NAME;
    } else {
        Q_UNREACHABLE();
    }

    const char *opt_extensions[] = {
        VK_EXT_HDR_METADATA_EXTENSION_NAME,
    };

    struct pl_log_params log_params = {
        .log_cb = placebo_log_cb,
        .log_level = PL_LOG_DEBUG,
    };
    placebo_log = pl_log_create(PL_API_VER, &log_params);

    struct pl_vk_inst_params vk_inst_params = {
        .debug = true,
        .extensions = vk_exts,
        .num_extensions = 2,
        .opt_extensions = opt_extensions,
        .num_opt_extensions = 1,
    };
    placebo_vk_inst = pl_vk_inst_create(placebo_log, &vk_inst_params);

    struct pl_vulkan_params vulkan_params = {
        .instance = placebo_vk_inst->instance,
        .get_proc_addr = placebo_vk_inst->get_proc_addr,
        PL_VULKAN_DEFAULTS
    };
    placebo_vulkan = pl_vulkan_create(placebo_log, &vulkan_params);

    struct pl_cache_params cache_params = {
        .log = placebo_log,
        .max_total_size = 10 << 20, // 10 MB
    };
    placebo_cache = pl_cache_create(&cache_params);
    pl_gpu_set_cache(placebo_vulkan->gpu, placebo_cache);
    FILE *file = fopen(shader_cache_path(), "rb");
    if (file) {
        pl_cache_load_file(placebo_cache, file);
        fclose(file);
    }

    placebo_renderer = pl_renderer_create(
        placebo_log,
        placebo_vulkan->gpu
    );

    QSurfaceFormat format;
    format.setAlphaBufferSize(8);
    setFormat(format);

    gl_context = new QOpenGLContext;
    gl_context->setFormat(format);
    if (!gl_context->create()) {
        qCCritical(chiakiGui) << "Failed to create GL context";
        return;
    }

#define GET_PROC(name_) \
    gl_funcs.name_ = reinterpret_cast<decltype(gl_funcs.name_)>(gl_context->getProcAddress(#name_)); \
    if (!gl_funcs.name_) { \
        qCCritical(chiakiGui) << "Failed to resolve" << #name_; \
        return; \
    }
    GET_PROC(glCreateMemoryObjectsEXT);
    GET_PROC(glDeleteMemoryObjectsEXT);
    GET_PROC(glMemoryObjectParameterivEXT);
    GET_PROC(glImportMemoryFdEXT);
    GET_PROC(glTexStorageMem2DEXT);
    GET_PROC(glIsMemoryObjectEXT);
    GET_PROC(glGenSemaphoresEXT);
    GET_PROC(glDeleteSemaphoresEXT);
    GET_PROC(glImportSemaphoreFdEXT);
    GET_PROC(glIsSempahoreEXT);
    GET_PROC(glWaitSemaphoreEXT);
    GET_PROC(glSignalSemaphoreEXT);
#undef GET_PROC

    gl_surface = new QOffscreenSurface;
    gl_surface->setFormat(gl_context->format());
    gl_surface->create();

    quick_render = new QQuickRenderControl;

    QQuickWindow::setDefaultAlphaBuffer(true);
    quick_window = new QQuickWindow(quick_render);
    quick_window->setColor(QColor(0, 0, 0, 0));

    qml_engine = new QQmlEngine(this);
    if (!qml_engine->incubationController())
        qml_engine->setIncubationController(quick_window->incubationController());
    connect(qml_engine, &QQmlEngine::quit, this, &QWindow::close);

    backend = new QmlBackend(settings, this);
    connect(backend, &QmlBackend::sessionChanged, this, [this](StreamSession *s) {
        session = s;
        chiaki_log_ctx = session ? session->GetChiakiLog() : nullptr;
        if (has_video) {
            has_video = false;
            setCursor(Qt::ArrowCursor);
            emit hasVideoChanged();
        }
    });

    render_thread = new QThread;
    render_thread->setObjectName("render");
    render_thread->start();

    quick_render->prepareThread(render_thread);
    quick_render->moveToThread(render_thread);
    gl_context->moveToThread(render_thread);

    connect(quick_render, &QQuickRenderControl::sceneChanged, this, [this]() {
        quick_need_sync = true;
        scheduleUpdate();
    });
    connect(quick_render, &QQuickRenderControl::renderRequested, this, [this]() {
        quick_need_render = true;
        scheduleUpdate();
    });

    update_timer = new QTimer(this);
    update_timer->setSingleShot(true);
    connect(update_timer, &QTimer::timeout, this, &QmlMainWindow::update);

    QMetaObject::invokeMethod(quick_render, [this]() {
        gl_context->makeCurrent(gl_surface);
        quick_render->initialize(gl_context);
    });
}

void QmlMainWindow::update()
{
    Q_ASSERT(QThread::currentThread() == QGuiApplication::instance()->thread());

    if (closing || update_scheduled)
        return;

    if (quick_need_sync) {
        quick_need_sync = false;
        quick_render->polishItems();
        QMetaObject::invokeMethod(quick_render, std::bind(&QmlMainWindow::sync, this), Qt::BlockingQueuedConnection);
    }

    update_timer->stop();
    update_scheduled = true;
    QMetaObject::invokeMethod(quick_render, std::bind(&QmlMainWindow::render, this));
}

void QmlMainWindow::scheduleUpdate()
{
    Q_ASSERT(QThread::currentThread() == QGuiApplication::instance()->thread());

    if (closing || !isExposed())
        return;

    if (!update_timer->isActive())
        update_timer->start(has_video ? 50 : 16);
}

void QmlMainWindow::createSwapchain()
{
    Q_ASSERT(QThread::currentThread() == render_thread);

    if (placebo_swapchain)
        return;

    VkResult err = VK_ERROR_UNKNOWN;
    if (QGuiApplication::platformName().startsWith("wayland")) {
        PFN_vkCreateWaylandSurfaceKHR createSurface = reinterpret_cast<PFN_vkCreateWaylandSurfaceKHR>(
                placebo_vk_inst->get_proc_addr(placebo_vk_inst->instance, "vkCreateWaylandSurfaceKHR"));

        VkWaylandSurfaceCreateInfoKHR surfaceInfo;
        memset(&surfaceInfo, 0, sizeof(surfaceInfo));
        surfaceInfo.sType = VK_STRUCTURE_TYPE_WAYLAND_SURFACE_CREATE_INFO_KHR;
        QPlatformNativeInterface* pni = QGuiApplication::platformNativeInterface();
        surfaceInfo.display = static_cast<struct wl_display*>(pni->nativeResourceForWindow("display", this));
        surfaceInfo.surface = static_cast<struct wl_surface*>(pni->nativeResourceForWindow("surface", this));
        err = createSurface(placebo_vk_inst->instance, &surfaceInfo, nullptr, &surface);
    } else if (QGuiApplication::platformName().startsWith("xcb")) {
        PFN_vkCreateXcbSurfaceKHR createSurface = reinterpret_cast<PFN_vkCreateXcbSurfaceKHR>(
                placebo_vk_inst->get_proc_addr(placebo_vk_inst->instance, "vkCreateXcbSurfaceKHR"));

        VkXcbSurfaceCreateInfoKHR surfaceInfo;
        memset(&surfaceInfo, 0, sizeof(surfaceInfo));
        surfaceInfo.sType = VK_STRUCTURE_TYPE_XCB_SURFACE_CREATE_INFO_KHR;
        QPlatformNativeInterface* pni = QGuiApplication::platformNativeInterface();
        surfaceInfo.connection = static_cast<xcb_connection_t*>(pni->nativeResourceForWindow("connection", this));
        surfaceInfo.window = static_cast<xcb_window_t>(winId());
        err = createSurface(placebo_vk_inst->instance, &surfaceInfo, nullptr, &surface);
    } else {
        Q_UNREACHABLE();
    }

    if (err != VK_SUCCESS)
        qFatal("Failed to create VkSurfaceKHR");

    struct pl_vulkan_swapchain_params swapchain_params = {
        .surface = surface,
        .present_mode = VK_PRESENT_MODE_MAILBOX_KHR,
    };
    placebo_swapchain = pl_vulkan_create_swapchain(placebo_vulkan, &swapchain_params);
}

void QmlMainWindow::destroySwapchain()
{
    Q_ASSERT(QThread::currentThread() == render_thread);

    if (!placebo_swapchain)
        return;

    destroySwapchainTextures();

    pl_swapchain_destroy(&placebo_swapchain);
    PFN_vkDestroySurfaceKHR destroySurface = reinterpret_cast<PFN_vkDestroySurfaceKHR>(
            placebo_vk_inst->get_proc_addr(placebo_vk_inst->instance, "vkDestroySurfaceKHR"));
    destroySurface(placebo_vk_inst->instance, surface, nullptr);
}

void QmlMainWindow::resizeSwapchain()
{
    Q_ASSERT(QThread::currentThread() == render_thread);

    if (!placebo_swapchain)
        createSwapchain();

    const QSize window_size(width() * devicePixelRatio(), height() * devicePixelRatio());
    if (window_size == swapchain_size)
        return;

    destroySwapchainTextures();

    swapchain_size = window_size;
    pl_swapchain_resize(placebo_swapchain, &swapchain_size.rwidth(), &swapchain_size.rheight());

    delete gl_fbo;
    gl_fbo = new QOpenGLFramebufferObject(swapchain_size);
    quick_window->setRenderTarget(gl_fbo);
}

void QmlMainWindow::updateSwapchain()
{
    Q_ASSERT(QThread::currentThread() == QGuiApplication::instance()->thread());

    if (closing)
        return;

    quick_item->setSize(size());
    quick_window->resize(size());

    QMetaObject::invokeMethod(quick_render, std::bind(&QmlMainWindow::resizeSwapchain, this), Qt::BlockingQueuedConnection);
    quick_render->polishItems();
    QMetaObject::invokeMethod(quick_render, std::bind(&QmlMainWindow::sync, this), Qt::BlockingQueuedConnection);
    update();
}

bool QmlMainWindow::getSwapchainTexture(pl_tex fbo, SwapchainTexture &t)
{
    if (swapchain_textures.contains(fbo)) {
        t = swapchain_textures[fbo];
        return true;
    }

    struct pl_tex_params tex_params = {
        .w = swapchain_size.width(),
        .h = swapchain_size.height(),
        .format = pl_find_fmt(placebo_vulkan->gpu, PL_FMT_UNORM, 4, 0, 0, PL_FMT_CAP_RENDERABLE),
        .sampleable = true,
        .renderable = true,
        .export_handle = PL_HANDLE_FD,
    };
    t.placebo_tex = pl_tex_create(placebo_vulkan->gpu, &tex_params);
    if (!t.placebo_tex) {
        qCCritical(chiakiGui) << "Failed to create placebo texture";
        return false;
    }

    gl_funcs.glCreateMemoryObjectsEXT(1, &t.gl_mem);
    GLint dedicated = GL_TRUE;
    gl_funcs.glMemoryObjectParameterivEXT(t.gl_mem, GL_DEDICATED_MEMORY_OBJECT_EXT, &dedicated);
    gl_funcs.glImportMemoryFdEXT(t.gl_mem, t.placebo_tex->shared_mem.size, GL_HANDLE_TYPE_OPAQUE_FD_EXT, dup(t.placebo_tex->shared_mem.handle.fd));

    gl_context->functions()->glDeleteTextures(1, &t.gl_tex);
    gl_context->functions()->glGenTextures(1, &t.gl_tex);
    gl_context->functions()->glBindTexture(GL_TEXTURE_2D, t.gl_tex);
    gl_context->functions()->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_TILING_EXT, GL_OPTIMAL_TILING_EXT);
    gl_funcs.glTexStorageMem2DEXT(GL_TEXTURE_2D, 1, GL_RGBA8, swapchain_size.width(), swapchain_size.height(), t.gl_mem, 0);
    gl_context->functions()->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    gl_context->functions()->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);

    if (!gl_funcs.glIsMemoryObjectEXT(t.gl_mem)) {
        qCCritical(chiakiGui) << "OpenGL image import failed";
        return false;
    }

    gl_context->functions()->glGenFramebuffers(1, &t.gl_fbo);
    gl_context->functions()->glBindFramebuffer(GL_FRAMEBUFFER, t.gl_fbo);
    gl_context->functions()->glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, t.gl_tex, 0);
    gl_context->functions()->glBindFramebuffer(GL_FRAMEBUFFER, 0);

    union pl_handle sem_in = {};
    struct pl_vulkan_sem_params sem_params_in = {
        .type = VK_SEMAPHORE_TYPE_BINARY,
        .export_handle = PL_HANDLE_FD,
        .out_handle = &sem_in,
    };
    t.vk_sem_in = pl_vulkan_sem_create(placebo_vulkan->gpu, &sem_params_in);

    union pl_handle sem_out = {};
    struct pl_vulkan_sem_params sem_params_out = {
        .type = VK_SEMAPHORE_TYPE_BINARY,
        .export_handle = PL_HANDLE_FD,
        .out_handle = &sem_out,
    };
    t.vk_sem_out = pl_vulkan_sem_create(placebo_vulkan->gpu, &sem_params_out);

    gl_funcs.glGenSemaphoresEXT(1, &t.gl_sem_in);
    gl_funcs.glGenSemaphoresEXT(1, &t.gl_sem_out);
    gl_funcs.glImportSemaphoreFdEXT(t.gl_sem_in, GL_HANDLE_TYPE_OPAQUE_FD_EXT, sem_in.fd);
    gl_funcs.glImportSemaphoreFdEXT(t.gl_sem_out, GL_HANDLE_TYPE_OPAQUE_FD_EXT, sem_out.fd);

    if (!gl_funcs.glIsSempahoreEXT(t.gl_sem_in) || !gl_funcs.glIsSempahoreEXT(t.gl_sem_out)) {
        qCCritical(chiakiGui) << "OpenGL semaphore import failed";
        return false;
    }

    swapchain_textures[fbo] = t;
    return true;
}

void QmlMainWindow::destroySwapchainTextures()
{
    if (swapchain_textures.isEmpty())
        return;

    // Need to make sure the semaphores are not currently being used
    pl_gpu_finish(placebo_vulkan->gpu);

    for (auto &t : swapchain_textures) {
        pl_tex_destroy(placebo_vulkan->gpu, &t.placebo_tex);
        pl_vulkan_sem_destroy(placebo_vulkan->gpu, &t.vk_sem_in);
        pl_vulkan_sem_destroy(placebo_vulkan->gpu, &t.vk_sem_out);

        gl_funcs.glDeleteMemoryObjectsEXT(1, &t.gl_mem);
        gl_funcs.glDeleteSemaphoresEXT(1, &t.gl_sem_in);
        gl_funcs.glDeleteSemaphoresEXT(1, &t.gl_sem_out);
        gl_context->functions()->glDeleteTextures(1, &t.gl_tex);
        gl_context->functions()->glDeleteFramebuffers(1, &t.gl_fbo);
    }

    swapchain_textures.clear();
}

void QmlMainWindow::sync()
{
    Q_ASSERT(QThread::currentThread() == render_thread);

    quick_need_render = quick_render->sync();
}

void QmlMainWindow::render()
{
    Q_ASSERT(QThread::currentThread() == render_thread);

    if (closing)
        return;

    update_scheduled = false;

    if (quick_need_render) {
        quick_need_render = false;
        quick_render->render();
    }

    frame_mutex.lock();
    if (next_frame || (!has_video && !keep_video)) {
        av_frame_free(&current_frame);
        std::swap(current_frame, next_frame);
    }
    frame_mutex.unlock();

    struct pl_swapchain_frame sw_frame = {};
    if (!pl_swapchain_start_frame(placebo_swapchain, &sw_frame)) {
        qCWarning(chiakiGui) << "Failed to start Placebo frame!";
        return;
    }

    SwapchainTexture tex;
    if (!getSwapchainTexture(sw_frame.fbo, tex)) {
        qCWarning(chiakiGui) << "Failed to get swapchain texture";
        return;
    }

    struct pl_frame target_frame = {};
    pl_frame_from_swapchain(&target_frame, &sw_frame);

    struct pl_vulkan_hold_params hold_params = {
        .tex = tex.placebo_tex,
        .layout = VK_IMAGE_LAYOUT_GENERAL,
        .qf = VK_QUEUE_FAMILY_EXTERNAL,
        .semaphore = {
            .sem = tex.vk_sem_in,
        }
    };
    pl_vulkan_hold_ex(placebo_vulkan->gpu, &hold_params);

    GLenum gl_layout = GL_LAYOUT_GENERAL_EXT;
    gl_funcs.glWaitSemaphoreEXT(tex.gl_sem_in, 0, 0, 1, &tex.gl_tex, &gl_layout);

    gl_context->functions()->glBindFramebuffer(GL_READ_FRAMEBUFFER, gl_fbo->handle());
    gl_context->functions()->glBindFramebuffer(GL_DRAW_FRAMEBUFFER, tex.gl_fbo);
    gl_context->extraFunctions()->glBlitFramebuffer(0, 0, swapchain_size.width(), swapchain_size.height(), 0, 0, swapchain_size.width(), swapchain_size.height(), GL_COLOR_BUFFER_BIT, GL_LINEAR);
    gl_context->functions()->glBindFramebuffer(GL_READ_FRAMEBUFFER, 0);
    gl_context->functions()->glBindFramebuffer(GL_DRAW_FRAMEBUFFER, 0);

    gl_funcs.glSignalSemaphoreEXT(tex.gl_sem_out, 0, 0, 1, &tex.gl_tex, &gl_layout);

    struct pl_vulkan_release_params release_params = {
        .tex = tex.placebo_tex,
        .layout = VK_IMAGE_LAYOUT_GENERAL,
        .qf = VK_QUEUE_FAMILY_EXTERNAL,
        .semaphore = {
            .sem = tex.vk_sem_out,
        }
    };
    pl_vulkan_release_ex(placebo_vulkan->gpu, &release_params);

    struct pl_overlay_part overlay_part = {
        .src = {0, 0, static_cast<float>(swapchain_size.width()), static_cast<float>(swapchain_size.height())},
        .dst = {0, static_cast<float>(swapchain_size.height()), static_cast<float>(swapchain_size.width()), 0},
    };
    struct pl_overlay overlay = {
        .tex = tex.placebo_tex,
        .repr = pl_color_repr_rgb,
        .color = pl_color_space_srgb,
        .parts = &overlay_part,
        .num_parts = 1,
    };
    target_frame.overlays = &overlay;
    target_frame.num_overlays = 1;

    struct pl_frame placebo_frame = {};
    if (current_frame) {
        struct pl_avframe_params avparams = {
            .frame = current_frame,
            .tex = placebo_tex,
        };
        bool mapped = pl_map_avframe_ex(placebo_vulkan->gpu, &placebo_frame, &avparams);
        if (!mapped) {
            qCWarning(chiakiGui) << "Failed to map AVFrame to Placebo frame!";
            return;
        }
        pl_rect2df crop = placebo_frame.crop;
        switch (video_mode) {
        case VideoMode::Normal:
            pl_rect2df_aspect_copy(&target_frame.crop, &crop, 0.0);
            break;
        case VideoMode::Stretch:
            // Nothing to do, target.crop already covers the full image
            break;
        case VideoMode::Zoom:
            pl_rect2df_aspect_copy(&target_frame.crop, &crop, 1.0);
            break;
        }
        pl_swapchain_colorspace_hint(placebo_swapchain, &placebo_frame.color);
    }

    const struct pl_render_params *render_params;
    switch (video_preset) {
    case VideoPreset::Fast:
        render_params = &pl_render_fast_params;
        break;
    case VideoPreset::Default:
        render_params = &pl_render_default_params;
        break;
    case VideoPreset::HighQuality:
        render_params = &pl_render_high_quality_params;
        break;
    }
    if (!pl_render_image(placebo_renderer, current_frame ? &placebo_frame : nullptr, &target_frame, render_params)) {
        qCWarning(chiakiGui) << "Failed to render Placebo frame!";
        pl_unmap_avframe(placebo_vulkan->gpu, &placebo_frame);
        return;
    }

    if (!pl_swapchain_submit_frame(placebo_swapchain)) {
        qCWarning(chiakiGui) << "Failed to submit Placebo frame!";
        pl_unmap_avframe(placebo_vulkan->gpu, &placebo_frame);
        return;
    }

    pl_swapchain_swap_buffers(placebo_swapchain);

    pl_unmap_avframe(placebo_vulkan->gpu, &placebo_frame);
}

bool QmlMainWindow::handleShortcut(QKeyEvent *event)
{
    if (!event->modifiers().testFlag(Qt::ControlModifier))
        return false;

    switch (event->key()) {
    case Qt::Key_F11:
        if (windowState() != Qt::WindowFullScreen)
            showFullScreen();
        else
            showNormal();
        return true;
    case Qt::Key_S:
        if (has_video)
            video_mode = video_mode == VideoMode::Stretch ? VideoMode::Normal : VideoMode::Stretch;
        return true;
    case Qt::Key_Z:
        if (has_video)
            video_mode = video_mode == VideoMode::Zoom ? VideoMode::Normal : VideoMode::Zoom;
        return true;
    case Qt::Key_M:
        if (session)
            session->ToggleMute();
        return true;
    case Qt::Key_Q:
        close();
        return true;
    default:
        return false;
    }
}

bool QmlMainWindow::event(QEvent *event)
{
    switch (event->type()) {
    case QEvent::MouseMove:
        if (session && !grab_input) {
            session->HandleMouseMoveEvent(static_cast<QMouseEvent*>(event), width(), height());
            return true;
        }
        QGuiApplication::sendEvent(quick_window, event);
        break;
    case QEvent::MouseButtonPress:
        if (static_cast<QMouseEvent*>(event)->source() != Qt::MouseEventNotSynthesized)
            return true;
        if (session && !grab_input) {
            session->HandleMousePressEvent(static_cast<QMouseEvent*>(event));
            return true;
        }
        QGuiApplication::sendEvent(quick_window, event);
        break;
    case QEvent::MouseButtonRelease:
        if (session && !grab_input) {
            session->HandleMouseReleaseEvent(static_cast<QMouseEvent*>(event));
            return true;
        }
        QGuiApplication::sendEvent(quick_window, event);
        break;
    case QEvent::KeyPress:
        if (handleShortcut(static_cast<QKeyEvent*>(event)))
            return true;
    case QEvent::KeyRelease:
        if (session && !grab_input) {
            session->HandleKeyboardEvent(static_cast<QKeyEvent*>(event));
            return true;
        }
        QGuiApplication::sendEvent(quick_window, event);
        break;
    case QEvent::TouchBegin:
    case QEvent::TouchUpdate:
    case QEvent::TouchEnd:
        if (session && !grab_input) {
            session->HandleTouchEvent(static_cast<QTouchEvent*>(event));
            return true;
        }
        QGuiApplication::sendEvent(quick_window, event);
        break;
    case QEvent::Close:
        if (!backend->closeRequested())
            return false;
        closing = true;
        QMetaObject::invokeMethod(quick_render, std::bind(&QmlMainWindow::destroySwapchain, this), Qt::BlockingQueuedConnection);
        break;
    default:
        break;
    }

    bool ret = QWindow::event(event);

    switch (event->type()) {
    case QEvent::Expose:
        if (isExposed())
            updateSwapchain();
        else
            QMetaObject::invokeMethod(quick_render, std::bind(&QmlMainWindow::destroySwapchain, this), Qt::BlockingQueuedConnection);
        break;
    case QEvent::Resize:
        if (isExposed())
            updateSwapchain();
        break;
    default:
        break;
    }

    return ret;
}
