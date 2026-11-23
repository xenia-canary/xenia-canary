/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Xenia Canary. All rights reserved.                          *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include <fcntl.h>
#include <poll.h>
#include <spawn.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <thread>
#include <vector>

#include "xenia/base/logging.h"
#include "xenia/ui/console_window.h"

extern char** environ;

namespace xe {
namespace ui {

namespace {

// Terminal emulators like gnome-terminal hand the command over to a server
// process, so file descriptors can't be inherited through them. Instead, the
// shell running in the new terminal reports its tty path through a FIFO, and
// that tty is then opened here. The shell exits, closing the window, when this
// process exits.
class TerminalConsoleWindow final : public ConsoleWindow {
 public:
  ~TerminalConsoleWindow() override { DestroyFifo(); }

  bool Open() override;

 private:
  struct Terminal {
    std::string program;
    // Empty if the command is passed directly.
    std::string exec_option;
  };

  enum class SpawnResult {
    kSucceeded,
    kNotAvailable,
    // Not trying other terminals, as that could open multiple windows.
    kTimedOut,
  };

  static constexpr std::chrono::seconds kTtyTimeout{5};

  static std::vector<Terminal> GetTerminalCandidates();

  bool CreateFifo();
  void DestroyFifo();
  SpawnResult SpawnTerminal(const Terminal& terminal);
  SpawnResult WaitForTtyPath(pid_t child);
  bool RedirectOutputToTty() const;

  std::string temp_dir_;
  std::string fifo_path_;
  int fifo_fd_ = -1;
  std::string tty_path_;
};

bool TerminalConsoleWindow::Open() {
  if (isatty(STDOUT_FILENO)) {
    return true;
  }

  if (!CreateFifo()) {
    return false;
  }

  SpawnResult result = SpawnResult::kNotAvailable;

  for (const Terminal& terminal : GetTerminalCandidates()) {
    result = SpawnTerminal(terminal);
    if (result != SpawnResult::kNotAvailable) {
      break;
    }
  }

  DestroyFifo();

  if (result != SpawnResult::kSucceeded) {
    XELOGW("enable_console: Failed to open a terminal window");
    return false;
  }

  return RedirectOutputToTty();
}

std::vector<TerminalConsoleWindow::Terminal>
TerminalConsoleWindow::GetTerminalCandidates() {
  std::vector<Terminal> terminals;

  const char* terminal_env = secure_getenv("TERMINAL");
  if (terminal_env && *terminal_env) {
    terminals.push_back({terminal_env, "-e"});
  }

  terminals.insert(terminals.end(), {
                                        {"xdg-terminal-exec", ""},
                                        {"x-terminal-emulator", "-e"},
                                        {"gnome-terminal", "--"},
                                        {"konsole", "-e"},
                                        {"xfce4-terminal", "-x"},
                                        {"kitty", ""},
                                        {"alacritty", "-e"},
                                        {"foot", ""},
                                        {"wezterm", "start"},
                                        {"xterm", "-e"},
                                    });

  return terminals;
}

bool TerminalConsoleWindow::CreateFifo() {
  const char* temp_root = secure_getenv("XDG_RUNTIME_DIR");
  if (!temp_root || !*temp_root) {
    temp_root = "/tmp";
  }

  std::string temp_dir = std::string(temp_root) + "/xenia-console-XXXXXX";
  if (!mkdtemp(temp_dir.data())) {
    XELOGW("enable_console: Failed to create a temporary directory");
    return false;
  }

  temp_dir_ = std::move(temp_dir);

  std::string fifo_path = temp_dir_ + "/tty";
  if (mkfifo(fifo_path.c_str(), 0600) != 0) {
    XELOGW("enable_console: Failed to create a FIFO");
    DestroyFifo();
    return false;
  }

  fifo_path_ = std::move(fifo_path);

  // O_RDWR keeps the FIFO from reporting EOF before the writer connects.
  fifo_fd_ = open(fifo_path_.c_str(), O_RDWR | O_NONBLOCK | O_CLOEXEC);
  if (fifo_fd_ < 0) {
    XELOGW("enable_console: Failed to open the FIFO");
    DestroyFifo();
    return false;
  }

  return true;
}

void TerminalConsoleWindow::DestroyFifo() {
  if (fifo_fd_ >= 0) {
    close(fifo_fd_);
    fifo_fd_ = -1;
  }

  if (!fifo_path_.empty()) {
    unlink(fifo_path_.c_str());
    fifo_path_.clear();
  }

  if (!temp_dir_.empty()) {
    rmdir(temp_dir_.c_str());
    temp_dir_.clear();
  }
}

TerminalConsoleWindow::SpawnResult TerminalConsoleWindow::SpawnTerminal(
    const Terminal& terminal) {
  // $1 - FIFO path, $2 - PID of this process.
  static const char kScript[] =
      "tty > \"$1\"; "
      "while kill -0 \"$2\" 2>/dev/null; do sleep 1; done";

  std::vector<std::string> args_storage;
  args_storage.push_back(terminal.program);

  if (!terminal.exec_option.empty()) {
    args_storage.push_back(terminal.exec_option);
  }

  args_storage.insert(
      args_storage.end(),
      {"sh", "-c", kScript, "sh", fifo_path_, std::to_string(getpid())});

  std::vector<char*> args;
  for (std::string& arg : args_storage) {
    args.push_back(arg.data());
  }

  args.push_back(nullptr);

  pid_t child;
  if (posix_spawnp(&child, terminal.program.c_str(), nullptr, nullptr,
                   args.data(), environ) != 0) {
    return SpawnResult::kNotAvailable;
  }

  return WaitForTtyPath(child);
}

TerminalConsoleWindow::SpawnResult TerminalConsoleWindow::WaitForTtyPath(
    pid_t child) {
  // Some terminal emulators (gnome-terminal) exit successfully right away after
  // handing the window over to a server, so only an error exit means failure.
  bool child_running = true;
  SpawnResult result = SpawnResult::kTimedOut;
  std::string line;
  auto deadline = std::chrono::steady_clock::now() + kTtyTimeout;

  while (std::chrono::steady_clock::now() < deadline) {
    if (child_running) {
      int status;
      if (waitpid(child, &status, WNOHANG) == child) {
        child_running = false;

        if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
          result = SpawnResult::kNotAvailable;
          break;
        }
      }
    }

    pollfd pfd = {fifo_fd_, POLLIN, 0};
    if (poll(&pfd, 1, 100) <= 0 || !(pfd.revents & POLLIN)) {
      continue;
    }

    char buf[256];
    ssize_t read_size = read(fifo_fd_, buf, sizeof(buf));
    if (read_size <= 0) {
      continue;
    }

    line.append(buf, size_t(read_size));

    size_t newline = line.find('\n');
    if (newline != std::string::npos) {
      line.resize(newline);

      if (!line.empty() && line[0] == '/') {
        tty_path_ = std::move(line);
        result = SpawnResult::kSucceeded;
      }

      break;
    }
  }

  if (child_running) {
    // For terminal emulators that stay in the foreground until the window is
    // closed, reap the process whenever it exits.
    std::thread([child]() {
      int status;
      waitpid(child, &status, 0);
    }).detach();
  }

  return result;
}

bool TerminalConsoleWindow::RedirectOutputToTty() const {
  int tty_fd = open(tty_path_.c_str(), O_WRONLY | O_NOCTTY | O_CLOEXEC);
  if (tty_fd < 0) {
    XELOGW("enable_console: Failed to open {}", tty_path_);
    return false;
  }

  std::fflush(stdout);
  std::fflush(stderr);

  dup2(tty_fd, STDOUT_FILENO);
  dup2(tty_fd, STDERR_FILENO);

  close(tty_fd);
  return true;
}

}  // namespace

std::unique_ptr<ConsoleWindow> ConsoleWindow::Create() {
  return std::make_unique<TerminalConsoleWindow>();
}

}  // namespace ui
}  // namespace xe
