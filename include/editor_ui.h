#pragma once

#include "cursor_sync.h"
#include "peer_manager.h"
#include "pipeline.h"

#include "ftxui/component/component.hpp"
#include "ftxui/component/screen_interactive.hpp"

#include <atomic>
#include <chrono>
#include <memory>
#include <mutex>
#include <string>

// Shared state for transient status-bar notifications (e.g. peer disconnected).
// Thread-safe: written from the peer-leave callback, read from the render
// thread.
struct NotifState {
  std::mutex mtx;
  std::string text;
  std::chrono::steady_clock::time_point expires{};
};

// Build the full-screen editor FTXUI component.
// cursorPos is shared with the network callback (set up in main) so that
// remote-op OT adjustments can atomically update the cursor from any thread.
// Quit is signalled by storing false into running and calling ExitLoopClosure.
// notif carries transient disconnect messages shown in the status bar.
ftxui::Component MakeEditor(Pipeline &pipeline, PeerManager &peerMgr,
                            CursorSync &cursorSync,
                            ftxui::ScreenInteractive &screen,
                            std::atomic<bool> &running,
                            std::shared_ptr<std::atomic<int>> cursorPos,
                            std::shared_ptr<NotifState> notif);
