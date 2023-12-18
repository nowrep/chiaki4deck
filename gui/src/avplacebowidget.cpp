// SPDX-License-Identifier: LicenseRef-AGPL-3.0-only-OpenSSL

#include <avplacebowidget.h>
#include <avplaceboframeuploader.h>
#include <streamsession.h>

#include <QThread>
#include <QTimer>
#include <QGuiApplication>
#include <QWindow>
#include <stdio.h>
#include <QImage>
#include <QImageReader>
#include <QPainter>
#include <QStandardPaths>

#define PL_LIBAV_IMPLEMENTATION 0
#include <libplacebo/utils/libav.h>

#include <xcb/xcb.h>
#include <vulkan/vulkan.h>
#include <vulkan/vulkan_xcb.h>
#include <vulkan/vulkan_wayland.h>
#include <qpa/qplatformnativeinterface.h>

static inline QString GetShaderCacheFile()
{
    return QStandardPaths::writableLocation(QStandardPaths::AppDataLocation) + "/pl_shader.cache";
}

static bool UploadImage(const QImage &img, pl_gpu gpu, struct pl_plane *out_plane, pl_tex *tex)
{
    struct pl_plane_data data = {
        .type = PL_FMT_UNORM,
        .width = img.width(),
        .height = img.height(),
        .pixel_stride = static_cast<size_t>(img.bytesPerLine() / img.width()),
        .pixels = img.constBits(),
    };

    for (int c = 0; c < 4; c++) {
        data.component_size[c] = img.pixelFormat().redSize();
        data.component_pad[c] = 0;
        data.component_map[c] = c;
    }

    return pl_upload_plane(gpu, out_plane, tex, &data);
}

static QRect DialogRect(const QSize &size, const QFontMetrics &fm, const QString &title, const QString &text)
{
    int w = std::max(fm.boundingRect(title).width(), fm.boundingRect(text).width());
    int h = std::max(fm.boundingRect(title).height(), fm.boundingRect(text).height()) * 6;
    return QRect((size.width() - w) / 2, (size.height() - h) / 2, w, h);
}

static QRect DialogButton(const QRect &dialog_rect, int button)
{
    int x = dialog_rect.x() + (button ? dialog_rect.width() / 2 : 0);
    int y = dialog_rect.bottom() - dialog_rect.height() * 0.35 + 4;
    return QRect(x, y, dialog_rect.width() / 2, dialog_rect.height() * 0.35);
}

AVPlaceboWidget::AVPlaceboWidget(StreamSession *session, ResolutionMode resolution_mode, PlaceboPreset preset, QWidget *window)
    : window(window), session(session), resolution_mode(resolution_mode)
{
    window->windowHandle()->installEventFilter(this);
    window->windowHandle()->setSurfaceType(QWindow::VulkanSurface);

    if (preset == PlaceboPreset::Default)
    {
        CHIAKI_LOGI(session->GetChiakiLog(), "Using placebo default preset");
        render_params = pl_render_default_params;
    }
    else if (preset == PlaceboPreset::Fast)
    {
        CHIAKI_LOGI(session->GetChiakiLog(), "Using placebo fast preset");
        render_params = pl_render_fast_params;
    }
    else
    {
        CHIAKI_LOGI(session->GetChiakiLog(), "Using placebo high quality preset");
        render_params = pl_render_high_quality_params;
    }

    const char *vk_exts[] = {
        VK_KHR_SURFACE_EXTENSION_NAME,
        nullptr,
    };

    QString platformName = QGuiApplication::platformName();
    if (platformName.startsWith("wayland")) {
        vk_exts[1] = VK_KHR_WAYLAND_SURFACE_EXTENSION_NAME;
    } else if (platformName.startsWith("xcb")) {
        vk_exts[1] = VK_KHR_XCB_SURFACE_EXTENSION_NAME;
    } else {
        Q_UNREACHABLE();
    }

    const char *opt_extensions[] = {
        VK_EXT_HDR_METADATA_EXTENSION_NAME,
    };

    struct pl_log_params log_params = {
        .log_cb = PlaceboLog,
        .log_priv = session->GetChiakiLog(),
        .log_level = PL_LOG_DEBUG,
    };
    placebo_log = pl_log_create(PL_API_VER, &log_params);

    struct pl_vk_inst_params vk_inst_params = {
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
    FILE *file = fopen(qPrintable(GetShaderCacheFile()), "rb");
    if (file) {
        pl_cache_load_file(placebo_cache, file);
        fclose(file);
    }
}

AVPlaceboWidget::~AVPlaceboWidget()
{
    ReleaseSwapchain();

    FILE *file = fopen(qPrintable(GetShaderCacheFile()), "wb");
    if (file) {
        pl_cache_save_file(placebo_cache, file);
        fclose(file);
    }
    pl_cache_destroy(&placebo_cache);
    pl_vulkan_destroy(&placebo_vulkan);
    pl_vk_inst_destroy(&placebo_vk_inst);
    pl_log_destroy(&placebo_log);
}

void AVPlaceboWidget::Stop() {
    ReleaseSwapchain();
}

bool AVPlaceboWidget::ShowError(const QString &title, const QString &message) {
    error_title = title;
    error_text = message;
    RenderPlaceholderIcon();
    QTimer::singleShot(5000, window, &QWidget::close);
    return true;
}

bool AVPlaceboWidget::ShowDisconnectDialog(const QString &title, const QString &message, std::function<void(bool)> cb) {
    dialog_title = title;
    dialog_text = message;
    dialog_cb = cb;
    RenderDisconnectDialog();
    window->setCursor(Qt::ArrowCursor);
    return true;
}

bool AVPlaceboWidget::QueueFrame(AVFrame *frame) {
    if (frame->decode_error_flags) {
        CHIAKI_LOGW(session->GetChiakiLog(), "Skip decode error!");
        av_frame_free(&frame);
        return false;
    }
    num_frames_total++;
    bool render = true;
    frames_mutex.lock();
    if (queued_frame) {
        CHIAKI_LOGV(session->GetChiakiLog(), "Dropped rendering frame!");
        num_frames_dropped++;
        av_frame_free(&queued_frame);
        render = false;
    }
    queued_frame = frame;
    frames_mutex.unlock();
    if (render) {
        QMetaObject::invokeMethod(render_thread->parent(), std::bind(&AVPlaceboWidget::RenderFrame, this));
    }
    stream_started = true;
    return true;
}

void AVPlaceboWidget::RenderFrame()
{
    struct pl_swapchain_frame sw_frame = {0};
    struct pl_frame placebo_frame = {0};
    struct pl_frame target_frame = {0};
    pl_tex overlay_tex = {0};
    struct pl_overlay_part overlay_part = {0};
    struct pl_overlay overlay = {0};

    frames_mutex.lock();
    AVFrame *frame = queued_frame;
    queued_frame = nullptr;
    frames_mutex.unlock();

    if (!frame) {
        CHIAKI_LOGE(session->GetChiakiLog(), "No frame to render!");
        return;
    }

    struct pl_avframe_params avparams = {
        .frame = frame,
        .tex = placebo_tex,
        .map_dovi = false,
    };

    bool mapped = pl_map_avframe_ex(placebo_vulkan->gpu, &placebo_frame, &avparams);
    av_frame_free(&frame);
    if (!mapped) {
        CHIAKI_LOGE(session->GetChiakiLog(), "Failed to map AVFrame to Placebo frame!");
        return;
    }
    // set colorspace hint
    struct pl_color_space hint = placebo_frame.color;
    pl_swapchain_colorspace_hint(placebo_swapchain, &hint);

    pl_rect2df crop;

    if (!pl_swapchain_start_frame(placebo_swapchain, &sw_frame)) {
        CHIAKI_LOGE(session->GetChiakiLog(), "Failed to start Placebo frame!");
        goto cleanup;
    }

    pl_frame_from_swapchain(&target_frame, &sw_frame);

    crop = placebo_frame.crop;
    switch (resolution_mode) {
        case ResolutionMode::Normal:
            pl_rect2df_aspect_copy(&target_frame.crop, &crop, 0.0);
            break;
        case ResolutionMode::Stretch:
            // Nothing to do, target.crop already covers the full image
            break;
        case ResolutionMode::Zoom:
            pl_rect2df_aspect_copy(&target_frame.crop, &crop, 1.0);
            break;
    }

    if (!overlay_img.isNull()) {
        if (!UploadImage(overlay_img, placebo_vulkan->gpu, nullptr, &overlay_tex)) {
            CHIAKI_LOGE(session->GetChiakiLog(), "Failed to upload QImage!");
            goto cleanup;
        }
        overlay_part.src = {0, 0, static_cast<float>(overlay_img.width()), static_cast<float>(overlay_img.height())};
        overlay_part.dst = overlay_part.src;
        overlay.tex = overlay_tex;
        overlay.repr = pl_color_repr_rgb;
        overlay.color = pl_color_space_srgb;
        overlay.parts = &overlay_part;
        overlay.num_parts = 1;
        target_frame.overlays = &overlay;
        target_frame.num_overlays = 1;
    }

    if (!pl_render_image(placebo_renderer, &placebo_frame, &target_frame,
                         &render_params)) {
        CHIAKI_LOGE(session->GetChiakiLog(), "Failed to render Placebo frame!");
        goto cleanup;
    }

    if (!pl_swapchain_submit_frame(placebo_swapchain)) {
        CHIAKI_LOGE(session->GetChiakiLog(), "Failed to submit Placebo frame!");
        goto cleanup;
    }
    pl_swapchain_swap_buffers(placebo_swapchain);

cleanup:
    pl_unmap_avframe(placebo_vulkan->gpu, &placebo_frame);
    pl_tex_destroy(placebo_vulkan->gpu, &overlay_tex);
}

void AVPlaceboWidget::RenderImage(const QImage &img)
{
    struct pl_frame target_frame = {0};
    struct pl_swapchain_frame sw_frame = {0};
    struct pl_plane plane = {0};
    pl_tex tex = {0};

    if (!placebo_renderer) {
        CHIAKI_LOGE(session->GetChiakiLog(), "No renderer!");
        return;
    }

    if (!pl_swapchain_start_frame(placebo_swapchain, &sw_frame)) {
        CHIAKI_LOGE(session->GetChiakiLog(), "Failed to start Placebo frame!");
        return;
    }

    pl_frame_from_swapchain(&target_frame, &sw_frame);

    if (!UploadImage(img, placebo_vulkan->gpu, &plane, &tex)) {
        CHIAKI_LOGE(session->GetChiakiLog(), "Failed to upload QImage!");
        return;
    }

    struct pl_frame image = {
        .num_planes = 1,
        .planes = { plane },
        .repr = pl_color_repr_rgb,
        .color = pl_color_space_srgb,
        .crop = {0, 0, static_cast<float>(img.width()), static_cast<float>(img.height())},
    };

    pl_render_image(placebo_renderer, &image, &target_frame, &render_params);
    pl_swapchain_submit_frame(placebo_swapchain);
    pl_swapchain_swap_buffers(placebo_swapchain);
    pl_tex_destroy(placebo_vulkan->gpu, &tex);
}

void AVPlaceboWidget::RenderPlaceholderIcon()
{
    QImage img(window->size() * window->devicePixelRatio(), QImage::Format_RGBA8888);
    img.fill(Qt::black);

    QImageReader logo_reader(":/icons/chiaki.svg");
    int logo_size = std::min(img.width(), img.height()) / 2;
    logo_reader.setScaledSize(QSize(logo_size, logo_size));
    QImage logo_img = logo_reader.read();
    QPainter p(&img);
    QRect imageRect((img.width() - logo_img.width()) / 2, (img.height() - logo_img.height()) / 2, logo_img.width(), logo_img.height());
    p.drawImage(imageRect, logo_img);
    if (!error_title.isEmpty()) {
        QFont f = p.font();
        f.setPixelSize(26 * window->devicePixelRatio());
        p.setPen(Qt::white);
        f.setBold(true);
        p.setFont(f);
        int title_height = QFontMetrics(f).boundingRect(error_title).height();
        int title_y = imageRect.bottom() + (img.height() - imageRect.bottom() - title_height * 5) / 2;
        p.drawText(QRect(0, title_y, img.width(), title_height), Qt::AlignCenter, error_title);
        f.setPixelSize(22 * window->devicePixelRatio());
        f.setBold(false);
        p.setFont(f);
        p.drawText(QRect(0, title_y + title_height + 10, img.width(), img.height()), Qt::AlignTop | Qt::AlignHCenter, error_text);
    }
    p.end();

    if (render_thread)
        QMetaObject::invokeMethod(render_thread->parent(), std::bind(&AVPlaceboWidget::RenderImage, this, img));
    else
        RenderImage(img);
}

void AVPlaceboWidget::RenderDisconnectDialog()
{
    QImage img(window->size() * window->devicePixelRatio(), QImage::Format_RGBA8888);
    img.fill(qRgba(30, 30, 30, 230));

    QPainter p(&img);
    QFont f = p.font();
    f.setPixelSize(26 * window->devicePixelRatio());
    p.setPen(Qt::white);
    f.setBold(true);
    p.setFont(f);
    QFontMetrics fm(f);
    dialog_rect = DialogRect(img.size(), fm, dialog_title, dialog_text);
    int title_height = fm.boundingRect(dialog_title).height();
    int title_y = dialog_rect.top() + title_height;
    p.fillRect(dialog_rect, Qt::black);
    // Title
    p.drawText(QRect(dialog_rect.left(), title_y, dialog_rect.width(), title_height), Qt::AlignCenter, dialog_title);
    f.setPixelSize(22 * window->devicePixelRatio());
    f.setBold(false);
    p.setFont(f);
    // Text
    p.drawText(QRect(dialog_rect.left(), title_y + title_height + 10, dialog_rect.width(), dialog_rect.height()), Qt::AlignTop | Qt::AlignHCenter, dialog_text);
    // Sleep button
    f.setBold(true);
    p.setFont(f);
    QRect button1_rect = DialogButton(dialog_rect, 0);
    p.fillRect(button1_rect, qRgb(10, 10, 60));
    p.drawText(button1_rect, Qt::AlignCenter, tr("⏻  SLEEP"));
    // No button
    QRect button2_rect = DialogButton(dialog_rect, 1);
    p.fillRect(button2_rect, qRgb(15, 15, 15));
    p.drawText(button2_rect, Qt::AlignCenter, tr("NO"));
    p.end();

    QMetaObject::invokeMethod(render_thread->parent(), [this, img]() { overlay_img = img; });
}

void AVPlaceboWidget::CreateSwapchain()
{
    VkResult err;
    if (QGuiApplication::platformName().startsWith("wayland")) {
        PFN_vkCreateWaylandSurfaceKHR createSurface = reinterpret_cast<PFN_vkCreateWaylandSurfaceKHR>(
                placebo_vk_inst->get_proc_addr(placebo_vk_inst->instance, "vkCreateWaylandSurfaceKHR"));

        VkWaylandSurfaceCreateInfoKHR surfaceInfo;
        memset(&surfaceInfo, 0, sizeof(surfaceInfo));
        surfaceInfo.sType = VK_STRUCTURE_TYPE_WAYLAND_SURFACE_CREATE_INFO_KHR;
        QPlatformNativeInterface* pni = QGuiApplication::platformNativeInterface();
        surfaceInfo.display = static_cast<struct wl_display*>(pni->nativeResourceForWindow("display", window->windowHandle()));
        surfaceInfo.surface = static_cast<struct wl_surface*>(pni->nativeResourceForWindow("surface", window->windowHandle()));
        err = createSurface(placebo_vk_inst->instance, &surfaceInfo, nullptr, &surface);
    } else if (QGuiApplication::platformName().startsWith("xcb")) {
        PFN_vkCreateXcbSurfaceKHR createSurface = reinterpret_cast<PFN_vkCreateXcbSurfaceKHR>(
                placebo_vk_inst->get_proc_addr(placebo_vk_inst->instance, "vkCreateXcbSurfaceKHR"));

        VkXcbSurfaceCreateInfoKHR surfaceInfo;
        memset(&surfaceInfo, 0, sizeof(surfaceInfo));
        surfaceInfo.sType = VK_STRUCTURE_TYPE_XCB_SURFACE_CREATE_INFO_KHR;
        QPlatformNativeInterface* pni = QGuiApplication::platformNativeInterface();
        surfaceInfo.connection = static_cast<xcb_connection_t*>(pni->nativeResourceForWindow("connection", window->windowHandle()));
        surfaceInfo.window = static_cast<xcb_window_t>(window->windowHandle()->winId());
        err = createSurface(placebo_vk_inst->instance, &surfaceInfo, nullptr, &surface);
    } else {
        Q_UNREACHABLE();
    }

    if (err != VK_SUCCESS)
        qFatal("Failed to create VkSurfaceKHR");

    struct pl_vulkan_swapchain_params swapchain_params = {
        .surface = surface,
        .present_mode = VK_PRESENT_MODE_FIFO_KHR,
    };
    placebo_swapchain = pl_vulkan_create_swapchain(placebo_vulkan, &swapchain_params);

    placebo_renderer = pl_renderer_create(
        placebo_log,
        placebo_vulkan->gpu
    );

    frame_uploader = new AVPlaceboFrameUploader(session, this);
    frame_uploader_thread = new QThread(this);
    frame_uploader_thread->setObjectName("Frame Uploader");
    frame_uploader->moveToThread(frame_uploader_thread);
    frame_uploader_thread->start();

    QObject *render_obj = new QObject();
    render_thread = new QThread(render_obj);
    render_thread->setObjectName("Render");
    render_thread->start();
    render_obj->moveToThread(render_thread);
}

void AVPlaceboWidget::ReleaseSwapchain()
{
    if (!frame_uploader_thread)
        return;

    frame_uploader_thread->quit();
    frame_uploader_thread->wait();
    delete frame_uploader_thread;
    frame_uploader_thread = nullptr;

    delete frame_uploader;
    frame_uploader = nullptr;

    render_thread->quit();
    render_thread->wait();
    delete render_thread->parent();
    render_thread = nullptr;

    for (int i = 0; i < 4; i++) {
        if (placebo_tex[i])
            pl_tex_destroy(placebo_vulkan->gpu, &placebo_tex[i]);
    }

    pl_renderer_destroy(&placebo_renderer);
    pl_swapchain_destroy(&placebo_swapchain);

    PFN_vkDestroySurfaceKHR destroySurface = reinterpret_cast<PFN_vkDestroySurfaceKHR>(
            placebo_vk_inst->get_proc_addr(placebo_vk_inst->instance, "vkDestroySurfaceKHR"));
    destroySurface(placebo_vk_inst->instance, surface, nullptr);
}

bool AVPlaceboWidget::eventFilter(QObject *object, QEvent *event)
{
    QPoint clickPos;

    if (event->type() == QEvent::Resize) {
        QResizeEvent *e = static_cast<QResizeEvent*>(event);
        if (!window->isVisible())
            return false;

        if (!placebo_renderer)
            CreateSwapchain();

        int width = e->size().width() * window->devicePixelRatio();
        int height = e->size().height() * window->devicePixelRatio();
        pl_swapchain_resize(placebo_swapchain, &width, &height);

        if (!stream_started)
            QMetaObject::invokeMethod(this, &AVPlaceboWidget::RenderPlaceholderIcon, Qt::QueuedConnection);

        if (!dialog_rect.isEmpty())
            QMetaObject::invokeMethod(this, &AVPlaceboWidget::RenderDisconnectDialog, Qt::QueuedConnection);
    } else if (event->type() == QEvent::MouseButtonPress) {
        QMouseEvent *e = static_cast<QMouseEvent*>(event);
        clickPos = e->pos();
    } else if (event->type() == QEvent::TouchBegin) {
        QTouchEvent *e = static_cast<QTouchEvent*>(event);
        clickPos = e->touchPoints().at(0).pos().toPoint();
    }

    if (!clickPos.isNull()) {
        if (!error_title.isEmpty()) {
            QTimer::singleShot(250, window, &QWidget::close);
            return true;
        }

        if (!dialog_rect.isEmpty()) {
            if (DialogButton(dialog_rect, 0).contains(clickPos)) {
                QTimer::singleShot(250, this, std::bind(dialog_cb, true));
                return true;
            }
            if (DialogButton(dialog_rect, 1).contains(clickPos)) {
                QTimer::singleShot(250, this, std::bind(dialog_cb, false));
                return true;
            }
            if (!dialog_rect.adjusted(-25, -25, 50, 50).contains(clickPos)) {
                dialog_title.clear();
                dialog_text.clear();
                dialog_rect = {};
                QMetaObject::invokeMethod(render_thread->parent(), [this]() { overlay_img = {}; });
                HideMouse();
                return true;
            }
        }
    }

    return false;
}

void AVPlaceboWidget::HideMouse()
{
	window->setCursor(Qt::BlankCursor);
}

void AVPlaceboWidget::ToggleZoom()
{
	if( resolution_mode == Zoom )
		resolution_mode = Normal;
	else
		resolution_mode = Zoom;
}

void AVPlaceboWidget::ToggleStretch()
{
	if( resolution_mode == Stretch )
		resolution_mode = Normal;
	else
		resolution_mode = Stretch;
}

void AVPlaceboWidget::PlaceboLog(void *user, pl_log_level level, const char *msg)
{
    ChiakiLog *log = (ChiakiLog*)user;
    if (!log) {
        return;
    }

    ChiakiLogLevel chiaki_level;
    switch (level)
    {
        case PL_LOG_NONE:
        case PL_LOG_TRACE:
        case PL_LOG_DEBUG:
            chiaki_level = CHIAKI_LOG_VERBOSE;
            break;
        case PL_LOG_INFO:
            chiaki_level = CHIAKI_LOG_INFO;
            break;
        case PL_LOG_WARN:
            chiaki_level = CHIAKI_LOG_WARNING;
            break;
        case PL_LOG_ERR:
        case PL_LOG_FATAL:
            chiaki_level = CHIAKI_LOG_ERROR;
            break;
    }

    chiaki_log(log, chiaki_level, "[libplacebo] %s", msg);
}
