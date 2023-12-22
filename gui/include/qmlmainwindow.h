#pragma once

#include "streamsession.h"

#include <QMutex>
#include <QWindow>
#include <QQuickWindow>
#include <QSurfaceFormat>
#include <QLoggingCategory>
#include <QOffscreenSurface>
#include <QOpenGLFramebufferObject>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libplacebo/options.h>
#include <libplacebo/vulkan.h>
#include <libplacebo/renderer.h>
#include <libplacebo/log.h>
#include <libplacebo/cache.h>
}

Q_DECLARE_LOGGING_CATEGORY(chiakiGui);

class Settings;
class StreamSession;
class QmlBackend;

class QmlMainWindow : public QWindow
{
    Q_OBJECT
    Q_PROPERTY(bool hasVideo READ hasVideo NOTIFY hasVideoChanged)
    Q_PROPERTY(int corruptedFrames READ corruptedFrames NOTIFY corruptedFramesChanged)
    Q_PROPERTY(bool keepVideo READ keepVideo WRITE setKeepVideo NOTIFY keepVideoChanged)
    Q_PROPERTY(VideoMode videoMode READ videoMode WRITE setVideoMode NOTIFY videoModeChanged)
    Q_PROPERTY(VideoPreset videoPreset READ videoPreset WRITE setVideoPreset NOTIFY videoPresetChanged)

public:
    enum class VideoMode {
        Normal,
        Stretch,
        Zoom
    };
    Q_ENUM(VideoMode);

    enum class VideoPreset {
        Fast,
        Default,
        HighQuality
    };
    Q_ENUM(VideoPreset);

    QmlMainWindow(Settings *settings);
    QmlMainWindow(const StreamSessionConnectInfo &connect_info);
    ~QmlMainWindow();

    bool hasVideo() const;
    int corruptedFrames() const;

    bool keepVideo() const;
    void setKeepVideo(bool keep);

    VideoMode videoMode() const;
    void setVideoMode(VideoMode mode);

    VideoPreset videoPreset() const;
    void setVideoPreset(VideoPreset mode);

    Q_INVOKABLE void grabInput();
    Q_INVOKABLE void releaseInput();

    void show();
    void presentFrame(AVFrame *frame);

    static QSurfaceFormat createSurfaceFormat();

signals:
    void hasVideoChanged();
    void corruptedFramesChanged();
    void keepVideoChanged();
    void videoModeChanged();
    void videoPresetChanged();
    void menuRequested();

private:
    struct SwapchainTexture {
        bool dirty = true;
        pl_tex placebo_tex = {};
        VkSemaphore vk_sem_in = VK_NULL_HANDLE;
        VkSemaphore vk_sem_out = VK_NULL_HANDLE;
        GLuint gl_mem = 0;
        GLuint gl_tex = 0;
        GLuint gl_sem_in = 0;
        GLuint gl_sem_out = 0;
        GLuint gl_fbo = 0;
    };

    void init(Settings *settings);
    void update();
    void scheduleUpdate();
    void createSwapchain();
    void destroySwapchain();
    void resizeSwapchain();
    void updateSwapchain();
    SwapchainTexture &getSwapchainTexture(pl_tex fbo);
    void destroySwapchainTextures();
    void sync();
    void render();
    bool handleShortcut(QKeyEvent *event);
    bool event(QEvent *event) override;
    QObject *focusObject() const override;

    bool has_video = false;
    bool keep_video = false;
    int grab_input = 0;
    int corrupted_frames = 0;
    VideoMode video_mode = VideoMode::Normal;
    VideoPreset video_preset = VideoPreset::HighQuality;

    bool closing = false;
    QmlBackend *backend = {};
    StreamSession *session = {};

    pl_cache placebo_cache = {};
    pl_log placebo_log = {};
    pl_vk_inst placebo_vk_inst = {};
    pl_vulkan placebo_vulkan = {};
    pl_swapchain placebo_swapchain = {};
    pl_renderer placebo_renderer = {};
    pl_tex placebo_tex[4] = {{}, {}, {}, {}};
    VkSurfaceKHR surface = VK_NULL_HANDLE;
    QSize swapchain_size;
    QMutex frame_mutex;
    QThread *frame_thread = {};
    QThread *render_thread = {};
    AVFrame *current_frame = {};
    AVFrame *next_frame = {};
    std::atomic<bool> render_scheduled = {false};

    QOpenGLContext *gl_context = {};
    QOffscreenSurface *gl_surface = {};
    QOpenGLFramebufferObject *gl_fbo = {};
    QQmlEngine *qml_engine = {};
    QQuickWindow *quick_window = {};
    QQuickRenderControl *quick_render = {};
    QQuickItem *quick_item = {};
    QTimer *update_timer = {};
    QHash<pl_tex, SwapchainTexture> swapchain_textures;
    std::atomic<bool> quick_need_sync = {false};
    std::atomic<bool> quick_need_render = {false};
    struct {
        PFNGLCREATEMEMORYOBJECTSEXTPROC glCreateMemoryObjectsEXT;
        PFNGLDELETEMEMORYOBJECTSEXTPROC glDeleteMemoryObjectsEXT;
        PFNGLMEMORYOBJECTPARAMETERIVEXTPROC glMemoryObjectParameterivEXT;
        PFNGLIMPORTMEMORYFDEXTPROC glImportMemoryFdEXT;
        PFNGLTEXSTORAGEMEM2DEXTPROC glTexStorageMem2DEXT;
        PFNGLISMEMORYOBJECTEXTPROC glIsMemoryObjectEXT;
        PFNGLGENSEMAPHORESEXTPROC glGenSemaphoresEXT;
        PFNGLDELETESEMAPHORESEXTPROC glDeleteSemaphoresEXT;
        PFNGLIMPORTSEMAPHOREFDEXTPROC glImportSemaphoreFdEXT;
        PFNGLISSEMAPHOREEXTPROC glIsSempahoreEXT;
        PFNGLWAITSEMAPHOREEXTPROC glWaitSemaphoreEXT;
        PFNGLSIGNALSEMAPHOREEXTPROC glSignalSemaphoreEXT;
        PFNGLGETUNSIGNEDBYTEI_VEXTPROC glGetUnsignedBytei_vEXT;
    } gl_funcs;

    friend class QmlBackend;
};
