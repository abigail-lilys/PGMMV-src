
# PGMMV Player — WebAssembly 引擎源码

移植到 WebAssembly 的 PGMMV 引擎与业务层，基于 cocos2d-x 3.17 + 旧版 SpiderMonkey。

> 构建配置、解密脚本与前端运行时位于 [PGMMV-WASM](https://github.com/abigail-lilys/PGMMV-WASM)。

## 移植要点与踩坑记录

| 阶段 | 要点 | 坑 |
|---|---|---|
| **一 · JS 引擎与平台层** | SpiderMonkey 34 交叉编译为 wasm32（关 JIT）；新增 `CC_PLATFORM_EMSCRIPTEN` 平台层 | 未开 COOP/COEP → `SharedArrayBuffer`/pthread 不可用；缺 `-sFULL_ES2=1` → WebGL1 下所有 `drawArrays` 报错；shader 精度不统一 → 公共 uniform 致链接失败（黑屏） |
| **二 · 业务代码兼容** | `-fdelayed-template-parsing` + `-fms-extensions` 兼容 MSVC 语法；`msvc_compat.h` 提供宏 | `Slope.h:59` 非 const 左值引用绑临时对象 → 改 `const Vec2&`；`ARRAYSIZE`、`__declspec` 需在 shim 补齐；`#if 0` 代码被剔除，调试易误判 |
| **三 · 运行时与多线程** | Worker 池扩至 40；`AudioEngine` 悬空指针判空；桩掉 `JavascriptManager::forceGC()`；`getrusage` 桩 | 场景切换 `SceneLayer` 并发高，24 Worker 耗尽死锁；音频 `map::operator[]` 误插空指针；缺 JS 绑定时 `forceGC` 遍历缺失对象越界 |
| **四 · JS 绑定与输入** | 恢复最小绑定（`register_all_cocos2dx` 等）；前端注入虚拟键盘 | 全套绑定会缺 UI/3D 符号 → 只留核心；`KeyboardEvent.keyCode` 只读 → 用 `Object.defineProperty` 覆写 |

## 编译

```bash
mkdir build && cd build
emcmake cmake ..
emmake make pgmmv_classes -j8
emmake make pgmmv -j8
```

> 环境准备见 PGMMV-WASM 仓库。