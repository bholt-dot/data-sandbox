---
# repo-sp5l
title: 'Viewer groundwork: SDL3 GPU window, SPIR-V shader pipeline, snapshot handoff'
status: todo
type: feature
created_at: 2026-09-25T01:08:58Z
updated_at: 2026-09-25T01:08:58Z
parent: repo-89dz
---

SDL3 (FetchContent, system package preferred when present), GLSL -> SPIR-V at build time via glslangValidator, GPU device + window + swapchain, a minimal pipeline drawing something, headless screenshot test on lavapipe + Xvfb, and the thread/snapshot handoff API for the shell to publish read-only World snapshots. Main thread owns the window.
