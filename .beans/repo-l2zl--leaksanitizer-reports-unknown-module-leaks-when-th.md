---
# repo-l2zl
title: LeakSanitizer reports <unknown module> leaks when the viewer exits
status: completed
type: bug
priority: normal
created_at: 2026-09-25T12:08:39Z
updated_at: 2026-09-25T12:08:39Z
parent: repo-89dz
---

Reported from playing belter --view on Arch with the dev/viewer (ASan) build: 2x128 byte direct leaks from <unknown module> on a driver thread (pthread_once).

## Summary of Changes
Diagnosis: not a leak in our code. The Vulkan loader dlclose()s the installable client driver at shutdown; the driver's process-lifetime allocations (reachable from its globals) become unreachable once it is unmapped, and LSan runs after that. Reproduced with lavapipe; preloading the driver (so it's never unloaded) makes the report disappear, preloading only the loader doesn't.
Fix: apps/viewer/src/module_pins.{hpp,cpp} — in ASan builds only, snapshot the loaded shared objects before SDL_Init and mark every object loaded afterwards RTLD_NODELETE (after claiming the window and after the first frame, for lazily loaded shader compilers). Leak detection stays fully on for our code; the GPU CTests no longer need ASAN_OPTIONS=detect_leaks=0.
Research: google/sanitizers#899 (unknown module traces), NVIDIA forum reports of the same Vulkan leak pattern, Mozilla bug 1304156; RTLD_NODELETE is the established workaround.
