#include "editor_ui.h"
#include "logger.h"

#include "ftxui/component/component.hpp"
#include "ftxui/component/event.hpp"
#include "ftxui/component/screen_interactive.hpp"
#include "ftxui/dom/elements.hpp"
#include "ftxui/screen/terminal.hpp"

#include <chrono>
#include <map>
#include <optional>
#include <string>
#include <unordered_map>
#include <utility>

using namespace ftxui;

static constexpr int MAX_LINE_WIDTH = Pipeline::MAX_LINE_WIDTH;

// Returns {line, col} for a byte offset within the document.
static std::pair<int, int> posToLineCol(const std::string &doc, int pos) {
  int line = 0, col = 0;
  int limit = std::min(pos, static_cast<int>(doc.size()));
  for (int i = 0; i < limit; ++i) {
    if (doc[i] == '\n') {
      ++line;
      col = 0;
    } else {
      ++col;
    }
  }
  return {line, col};
}

// Byte offset of the first character on |lineIdx|.
static int lineStartOffset(const std::string &doc, int lineIdx) {
  int cur = 0, off = 0;
  while (cur < lineIdx) {
    auto nl = doc.find('\n', off);
    if (nl == std::string::npos)
      return static_cast<int>(doc.size());
    off = static_cast<int>(nl) + 1;
    ++cur;
  }
  return off;
}

// Byte offset of the '\n' that ends |lineIdx|, or doc.size() on the last line.
static int lineEndOffset(const std::string &doc, int lineIdx) {
  int start = lineStartOffset(doc, lineIdx);
  auto nl = doc.find('\n', start);
  return (nl == std::string::npos) ? static_cast<int>(doc.size())
                                   : static_cast<int>(nl);
}

// Total number of lines (always >= 1).
static int lineCount(const std::string &doc) {
  int n = 1;
  for (char c : doc)
    if (c == '\n')
      ++n;
  return n;
}

// Clamps the horizontal scroll offset so the cursor column is always visible.
static void adjustHScroll(int &hScroll, int curCol, int termWidth) {
  if (curCol < hScroll)
    hScroll = curCol;
  else if (curCol - hScroll >= termWidth)
    hScroll = curCol - termWidth + 1;
}

// Assigns a stable color to a remote peer based on their site ID.
static Color remCursorColor(uint32_t siteID) {
  static const Color kPalette[] = {
      Color::Red, Color::Green, Color::Yellow, Color::Magenta, Color::Cyan,
  };
  return kPalette[siteID % 5];
}

// Renders one line with any number of local and remote cursor highlights.
// localVisCol < 0 means no local cursor on this line.
// remCols is a list of (visible column, Color) for remote cursors on this line.
static Element renderLine(const std::string &visible, int localVisCol,
                          const std::vector<std::pair<int, Color>> &remCols,
                          bool isLastLine) {
  int len = static_cast<int>(visible.size());

  std::map<int, std::optional<Color>> cursorAt;
  for (auto &[col, clr] : remCols)
    if (col >= 0 && col <= len)
      cursorAt[col] = clr;
  if (localVisCol >= 0 && localVisCol <= len)
    cursorAt[localVisCol] = std::nullopt;

  if (cursorAt.empty())
    return text(visible.empty() && isLastLine ? " " : visible);

  Elements elems;
  int pos = 0;
  for (auto &[col, clrOpt] : cursorAt) {
    if (col > pos)
      elems.push_back(text(visible.substr(pos, col - pos)));
    std::string ch = (col < len) ? std::string(1, visible[col]) : " ";
    if (!clrOpt)
      elems.push_back(text(ch) | inverted);
    else
      elems.push_back(text(ch) | bgcolor(*clrOpt) | color(Color::Black));
    pos = col + 1;
  }
  if (pos <= len)
    elems.push_back(text(visible.substr(pos)));
  if (elems.empty())
    elems.push_back(text(isLastLine ? " " : ""));

  return hbox(std::move(elems));
}

// Builds the scrollable text area.
// remCursorMap maps (line, col) → Color for every remote cursor.
static Element
renderTextArea(const std::string &doc, int curLine, int curCol, int hScroll,
               int termWidth,
               const std::map<std::pair<int, int>, Color> &remCursorMap) {

  Elements lineElems;
  int lineIdx = 0;
  std::string cur;

  auto flushLine = [&](bool isLastLine) {
    std::string visible;
    if (static_cast<int>(cur.size()) > hScroll)
      visible = cur.substr(hScroll, termWidth);

    int localVisCol = (lineIdx == curLine) ? (curCol - hScroll) : -1;

    std::vector<std::pair<int, Color>> remOnLine;
    for (auto &[lc, clr] : remCursorMap) {
      if (lc.first == lineIdx)
        remOnLine.push_back({lc.second - hScroll, clr});
    }

    lineElems.push_back(
        renderLine(visible, localVisCol, remOnLine, isLastLine));

    cur.clear();
    ++lineIdx;
  };

  for (char c : doc) {
    if (c == '\n')
      flushLine(false);
    else
      cur += c;
  }
  flushLine(true);

  return vbox(std::move(lineElems)) | flex;
}

// Builds the status bar shown at the bottom of the screen.
static Element renderStatusBar(PeerManager &peerMgr,
                               std::shared_ptr<NotifState> notif) {
  auto peers = peerMgr.activePeers();
  std::string connStr =
      peers.empty() ? "No peers" : std::to_string(peers.size()) + " peer(s)";

  std::string label = "  " + connStr +
                      "  |  Site: " + siteToHex(peerMgr.siteID()) +
                      "  |  Esc to quit  ";

  // Check for a live disconnect notification and append it.
  std::string notifText;
  {
    std::lock_guard<std::mutex> lk(notif->mtx);
    if (std::chrono::steady_clock::now() < notif->expires)
      notifText = notif->text;
  }

  if (notifText.empty())
    return text(label) | bold | bgcolor(Color::Blue) | color(Color::White);

  return hbox({
      text(label) | bold | bgcolor(Color::Blue) | color(Color::White),
      text("  " + notifText + "  ") | bold | bgcolor(Color::Yellow) |
          color(Color::Black),
  });
}

// Combines the text area and status bar into the full screen layout.
static Component makeRenderer(Pipeline &pipeline, PeerManager &peerMgr,
                              CursorSync &cursorSync,
                              std::shared_ptr<std::atomic<int>> cursorPos,
                              std::shared_ptr<int> hScroll,
                              std::shared_ptr<NotifState> notif) {
  return Renderer(
      [&pipeline, &peerMgr, &cursorSync, cursorPos, hScroll, notif] {
        std::string doc = pipeline.getDocument();
        int termWidth = Terminal::Size().dimx;

        int pos = std::min(cursorPos->load(), static_cast<int>(doc.size()));
        auto [curLine, curCol] = posToLineCol(doc, pos);

        adjustHScroll(*hScroll, curCol, termWidth);

        std::map<std::pair<int, int>, Color> remCursorMap;
        for (auto &[siteID, rpos] : cursorSync.getRemoteCursors()) {
          int clamped = std::min(rpos, static_cast<int>(doc.size()));
          auto [rl, rc] = posToLineCol(doc, clamped);
          remCursorMap[{rl, rc}] = remCursorColor(siteID);
        }

        return vbox({renderTextArea(doc, curLine, curCol, *hScroll, termWidth,
                                    remCursorMap),
                     separator(), renderStatusBar(peerMgr, notif)});
      });
}

// Wraps |inner| with a keyboard event handler that drives the editor.
static Component makeEventHandler(Component inner, Pipeline &pipeline,
                                  CursorSync &cursorSync,
                                  ScreenInteractive &screen,
                                  std::atomic<bool> &running,
                                  std::shared_ptr<std::atomic<int>> cursorPos) {
  return CatchEvent(
      inner,
      [&pipeline, &cursorSync, &screen, &running,
       cursorPos](Event event) -> bool {
        std::string doc = pipeline.getDocument();
        int pos = std::min(cursorPos->load(), static_cast<int>(doc.size()));
        auto setPos = [&](int p) {
          cursorPos->store(p);
          pos = p;
          cursorSync.broadcast(p);
        };
        auto [curLine, curCol] = posToLineCol(doc, pos);

        if (event == Event::Escape) {
          running.store(false);
          screen.ExitLoopClosure()();
          return true;
        }

        if (event == Event::ArrowLeft) {
          if (pos > 0)
            setPos(pos - 1);
          return true;
        }

        if (event == Event::ArrowRight) {
          if (pos < static_cast<int>(doc.size()))
            setPos(pos + 1);
          return true;
        }

        if (event == Event::ArrowUp) {
          if (curLine > 0) {
            int prevStart = lineStartOffset(doc, curLine - 1);
            int prevEnd = lineEndOffset(doc, curLine - 1);
            setPos(prevStart + std::min(curCol, prevEnd - prevStart));
          }
          return true;
        }

        if (event == Event::ArrowDown) {
          if (curLine + 1 < lineCount(doc)) {
            int nextStart = lineStartOffset(doc, curLine + 1);
            int nextEnd = lineEndOffset(doc, curLine + 1);
            setPos(nextStart + std::min(curCol, nextEnd - nextStart));
          }
          return true;
        }

        if (event == Event::Home) {
          setPos(lineStartOffset(doc, curLine));
          return true;
        }

        if (event == Event::End) {
          setPos(lineEndOffset(doc, curLine));
          return true;
        }

        if (event == Event::Backspace) {
          if (pos > 0) {
            pipeline.localDelete(pos - 1);
            setPos(pos - 1);
          }
          return true;
        }

        if (event == Event::Delete) {
          if (pos < static_cast<int>(doc.size()))
            pipeline.localDelete(pos);
          return true;
        }

        if (event == Event::Return) {
          pipeline.localInsert(pos, '\n');
          setPos(pos + 1);
          return true;
        }

        if (event.is_character()) {
          const std::string &ch = event.character();
          auto c = static_cast<unsigned char>(ch.empty() ? 0 : ch[0]);
          if (c >= 32 && c < 127) {
            int lineLen =
                lineEndOffset(doc, curLine) - lineStartOffset(doc, curLine);

            if (curCol >= MAX_LINE_WIDTH) {
              if (pos < static_cast<int>(doc.size())) {
                setPos(pos + 1);
              } else {
                pipeline.localInsert(pos, '\n');
                setPos(pos + 1);
              }
              doc = pipeline.getDocument();
              auto [nl, nc] = posToLineCol(doc, pos);
              curLine = nl;
              curCol = nc;
            } else if (lineLen >= MAX_LINE_WIDTH) {
              pipeline.localInsert(pos, static_cast<char>(c));
              setPos(pos + 1);
              doc = pipeline.getDocument();

              int wrapPos = lineStartOffset(doc, curLine) + MAX_LINE_WIDTH;
              int lineEnd = lineEndOffset(doc, curLine);

              if (lineEnd < static_cast<int>(doc.size())) {
                char overflow = doc[wrapPos];
                pipeline.localDelete(wrapPos);
                pipeline.localInsert(wrapPos + 1, overflow);
              } else {
                pipeline.localInsert(wrapPos, '\n');
                if (pos >= wrapPos)
                  setPos(pos + 1);
              }
              return true;
            }

            pipeline.localInsert(pos, static_cast<char>(c));
            setPos(pos + 1);
            return true;
          }
        }

        return false;
      });
}

Component MakeEditor(Pipeline &pipeline, PeerManager &peerMgr,
                     CursorSync &cursorSync, ScreenInteractive &screen,
                     std::atomic<bool> &running,
                     std::shared_ptr<std::atomic<int>> cursorPos,
                     std::shared_ptr<NotifState> notif) {
  auto hScroll = std::make_shared<int>(0);
  auto renderer =
      makeRenderer(pipeline, peerMgr, cursorSync, cursorPos, hScroll, notif);
  return makeEventHandler(renderer, pipeline, cursorSync, screen, running,
                          cursorPos);
}
