#ifndef PGMMV_DYNAMIC_FPS_PROBE_H
#define PGMMV_DYNAMIC_FPS_PROBE_H

#include <cstdint>
#if defined(__EMSCRIPTEN__)
#include <chrono>
#endif

namespace agtk { namespace dynamic_fps_probe {
enum class Bucket : unsigned char {
    SceneLayerUpdate,
    PlayerUpdate,
    PlayerTraversal,
    PlayerNodeVisits,
    ImagePlayerUpdate,
    GifPlayerUpdate,
    GifFrameSeek,
    GifAnimationAdvance,
    GifBitmapGet,
    GifTextureUpload,
    SSPlayerUpdate,
    AnimationMotionUpdate,
    PlayerUpdateTail,
    PlayerCenterUpdate,
    PlayerActionSprites,
    PlayerRenderTextureUpdate,
    PlayerShaderCleanup,
    DirectorDrawScene,
    DirectorPollEvents,
    DirectorScheduleUpdate,
    DirectorClear,
    DirectorSceneVisit,
    DirectorNotificationVisit,
    DirectorRendererRender,
    DirectorAfterDraw,
    DirectorSwapBuffers,
    SceneRender,
    NodeVisitCount,
    NodeSort,
    NodeSortActual,
    NodeProcessFlags,
    NodeBaseDraw,
    SpriteDraw,
    NodeVisitExclusive,
    NodeVirtualDraw,
    NodeVirtualDrawCount,
    NodeVisitMatrix,
    NodeVisitCameraCheck,
    SceneCameraApply,
    SceneCameraClear,
    SceneNodeVisit,
    SceneCameraRenderer,
    SceneCameraRestore,
    RendererQueueSort,
    RenderQueueSortOne,
    RendererVisitQueue,
    RendererClean,
    RenderQueueSaveState,
    RenderQueueRestoreState,
    RenderQueueCommandCount,
    RendererTriangleBatch,
    RendererFlush,
    CameraViewportApply,
    CameraViewportRestore,
    Count
};

#if defined(__EMSCRIPTEN__)
void record(Bucket bucket, std::uint64_t ns) noexcept;
void count(Bucket bucket) noexcept;
void reportIfDue() noexcept;

class ScopedTimer {
public:
    explicit ScopedTimer(Bucket bucket) noexcept
        : _bucket(bucket), _start(std::chrono::steady_clock::now()) {}
    ~ScopedTimer() noexcept {
        const auto elapsed = std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now() - _start).count();
        record(_bucket, elapsed > 0 ? static_cast<std::uint64_t>(elapsed) : 0);
    }
    ScopedTimer(const ScopedTimer&) = delete;
    ScopedTimer& operator=(const ScopedTimer&) = delete;
private:
    Bucket _bucket;
    std::chrono::steady_clock::time_point _start;
};

class ScopedExclusiveTimer {
public:
    explicit ScopedExclusiveTimer(Bucket bucket) noexcept;
    ~ScopedExclusiveTimer() noexcept;
    ScopedExclusiveTimer(const ScopedExclusiveTimer&) = delete;
    ScopedExclusiveTimer& operator=(const ScopedExclusiveTimer&) = delete;
private:
    Bucket _bucket;
    std::chrono::steady_clock::time_point _start;
    std::uint64_t _childNs;
    ScopedExclusiveTimer* _parent;
};

class ReportOnExit {
public:
    ~ReportOnExit() noexcept { reportIfDue(); }
};
#else
inline void count(Bucket) noexcept {}
class ScopedTimer {
public:
    explicit ScopedTimer(Bucket) noexcept {}
};
class ScopedExclusiveTimer {
public:
    explicit ScopedExclusiveTimer(Bucket) noexcept {}
};
class ReportOnExit {};
#endif
}}

#endif
