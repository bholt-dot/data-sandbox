#pragma once

// Thread-safe handoff between the game shell and the viewer window.
//
// Contract:
//   * Threads. The viewer window runs on the process's main thread (SDL requires video and
//     events there on some platforms); the shell runs on another thread. Both hold a reference
//     to one ViewerLink that outlives them both. Every member is safe to call from any thread.
//   * Snapshots. The shell calls publish() after each command that may change what is shown
//     (typically after every command). A snapshot is immutable once published and is shared by
//     shared_ptr, so the viewer keeps drawing an older one for as long as it holds it and never
//     blocks the shell for longer than a pointer swap. Only the latest snapshot matters:
//     publishing faster than the viewer renders simply drops intermediate ones.
//   * Closing, shell -> viewer. When the shell exits (quit, end of script, stdin closed) it
//     calls request_close(); the viewer notices within a frame, closes its window and returns
//     from run_viewer().
//   * Closing, viewer -> shell. When the window closes for any reason (user closed it, Esc,
//     a GPU failure, the frame budget of a screenshot run) run_viewer() calls
//     notify_viewer_closed() before returning. The shell polls viewer_closed() between
//     commands to decide whether to keep publishing; a shell may keep running headless after
//     the window is gone.
//   * Neither side ever waits on the other, so a blocked shell (waiting for input) cannot stall
//     the window, and a stalled window cannot block the shell.

#include "viewer/view_snapshot.hpp"

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>

namespace viewer {

class ViewerLink {
public:
    // Replaces the current snapshot. A null snapshot is ignored.
    void publish(std::shared_ptr<const ViewSnapshot> snapshot);
    // The most recently published snapshot, or null if nothing has been published yet.
    std::shared_ptr<const ViewSnapshot> latest() const;
    // Number of snapshots published so far; lets a reader skip work when nothing changed.
    std::uint64_t generation() const;

    // Shell -> viewer: please close the window.
    void request_close() { close_requested_.store(true, std::memory_order_release); }
    bool close_requested() const { return close_requested_.load(std::memory_order_acquire); }

    // Viewer -> shell: the window has closed (called by run_viewer on exit).
    void notify_viewer_closed() { viewer_closed_.store(true, std::memory_order_release); }
    bool viewer_closed() const { return viewer_closed_.load(std::memory_order_acquire); }

private:
    mutable std::mutex mutex_;
    std::shared_ptr<const ViewSnapshot> latest_; // guarded by mutex_
    std::uint64_t generation_ = 0;               // guarded by mutex_
    std::atomic<bool> close_requested_{false};
    std::atomic<bool> viewer_closed_{false};
};

} // namespace viewer
