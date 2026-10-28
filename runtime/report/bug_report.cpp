// SpeedBreaker runtime. GPL-3.0-or-later (see COPYING). See report.h.
//
// Save Bug Report (Settings > Advanced): one .zip a tester can attach to a
// message, with the recent session logs, crash and hang reports,
// settings.toml, the system summary and a screenshot of the last game
// frame. It goes to the Desktop when there is one (SteamOS has one, and
// Desktop Mode's file manager shows it), else the user data folder; on iOS
// to the app's Documents, which the Files app shows (On My iPhone/iPad >
// SpeedBreaker). NFSMW_BUG_REPORT_DIR=<dir> puts it there instead (tests).
// Its name carries the version: speedbreaker-bug-report-v0.1.0-<date>_<time>.zip.
#include <stdafx.h>
#include "report.h"
#include "internal.h"
#include "zip.h"

#include <cpu/guest_thread.h>
#include <gpu/renderer.h>
#include <ui/ui.h>
#include <user/paths.h>
#include <user/settings.h>
#include <video/presenter.h>

#include <cctype>
#include <deque>
#include <fstream>
#include <sstream>

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

namespace report
{
    namespace
    {
        std::atomic<BugReportState> s_state{ BugReportState::Idle };
        std::shared_future<video::CapturedFrame> s_menuFrame;  // main thread
        std::mutex s_messageMutex;
        std::string s_message;

        void SetMessage(std::string text)
        {
            std::lock_guard lock(s_messageMutex);
            s_message = std::move(text);
        }

        std::filesystem::path DesktopFolder()
        {
            const char* home = std::getenv("HOME");
            if (!home || !*home)
                return {};
            std::filesystem::path desktop = std::filesystem::path(home) / "Desktop";
#ifdef __linux__
            // A localized desktop folder: XDG_DESKTOP_DIR="$HOME/Schreibtisch"
            // in user-dirs.dirs.
            const char* config = std::getenv("XDG_CONFIG_HOME");
            std::ifstream dirs(std::filesystem::path(config && *config ? config : std::string(home) + "/.config") / "user-dirs.dirs");
            for (std::string line; std::getline(dirs, line);)
                if (line.starts_with("XDG_DESKTOP_DIR="))
                {
                    std::string value = line.substr(16);
                    if (value.size() >= 2 && value.front() == '"' && value.back() == '"')
                        value = value.substr(1, value.size() - 2);
                    if (value.starts_with("$HOME"))
                        value = std::string(home) + value.substr(5);
                    if (!value.empty() && value != home && value != std::string(home) + "/")
                        desktop = value;
                }
#endif
            std::error_code ec;
            if (std::filesystem::is_directory(desktop, ec) && access(desktop.c_str(), W_OK) == 0)
                return desktop;
            return {};
        }

        // iOS: the app's Documents folder (HOME/Documents), the one the
        // Files app shows (UIFileSharingEnabled); the data folder in
        // Library isn't visible there. Empty elsewhere.
        std::filesystem::path FilesAppFolder()
        {
#if defined(__APPLE__) && TARGET_OS_IOS
            if (const char* home = std::getenv("HOME"); home && *home)
            {
                std::filesystem::path documents = std::filesystem::path(home) / "Documents";
                std::error_code ec;
                std::filesystem::create_directories(documents, ec);
                if (std::filesystem::is_directory(documents, ec))
                    return documents;
            }
#endif
            return {};
        }

        // Where a saved report is, for the player: on iOS the Files app's
        // name for it; elsewhere the path with the home folder as "~".
        std::string Describe(const std::filesystem::path& path)
        {
#if defined(__APPLE__) && TARGET_OS_IOS
            if (std::filesystem::path files = FilesAppFolder(); !files.empty() && path.parent_path() == files)
                return "the Files app, armb02 folder: " + path.filename().string();
#endif
            return ScrubHome(path.string());
        }

        std::filesystem::path OutputFolder()
        {
            if (const char* v = std::getenv("NFSMW_BUG_REPORT_DIR"); v && *v)
                return v;
            if (std::filesystem::path files = FilesAppFolder(); !files.empty())
                return files;
            std::filesystem::path desktop = DesktopFolder();
            return desktop.empty() ? GetUserPath() : desktop;
        }

        bool ReadWhole(const std::filesystem::path& path, std::string& text)
        {
            std::ifstream in(path, std::ios::binary);
            if (!in)
                return false;
            std::ostringstream buffer;
            buffer << in.rdbuf();
            text = buffer.str();
            return true;
        }

        time_t ModifiedTime(const std::filesystem::path& path)
        {
            struct stat st{};
            return stat(path.c_str(), &st) == 0 ? st.st_mtime : time(nullptr);
        }

        // Halved (2x2 box filter) while wider than 2560: a 3440x1440 frame
        // is 1720x720, about 2 MB as a PNG instead of 7, and still shows
        // what's wrong.
        std::vector<uint8_t> Screenshot(const video::CapturedFrame& frame)
        {
            if (frame.rgb.empty() || frame.width == 0 || frame.height == 0)
                return {};
            uint32_t w = frame.width, h = frame.height;
            std::vector<uint8_t> pixels = frame.rgb;
            while (w > 2560 && h >= 2)
            {
                uint32_t hw = w / 2, hh = h / 2;
                std::vector<uint8_t> half(size_t(hw) * hh * 3);
                for (uint32_t y = 0; y < hh; y++)
                    for (uint32_t x = 0; x < hw; x++)
                        for (int c = 0; c < 3; c++)
                        {
                            auto at = [&](uint32_t sx, uint32_t sy) { return uint32_t(pixels[(size_t(sy) * w + sx) * 3 + size_t(c)]); };
                            half[(size_t(y) * hw + x) * 3 + size_t(c)] = uint8_t(
                                (at(2 * x, 2 * y) + at(2 * x + 1, 2 * y) + at(2 * x, 2 * y + 1) + at(2 * x + 1, 2 * y + 1) + 2) / 4);
                        }
                pixels.swap(half);
                w = hw;
                h = hh;
            }
            return EncodePng(pixels.data(), w, h);
        }

        // All one colour (a fade, a loading screen's black): a screenshot
        // with nothing on it.
        bool Blank(const video::CapturedFrame& frame)
        {
            if (frame.rgb.size() < 3)
                return true;
            for (size_t i = 0; i + 2 < frame.rgb.size(); i += 3 * 97)
                for (int c = 0; c < 3; c++)
                    if (std::abs(int(frame.rgb[i + size_t(c)]) - int(frame.rgb[size_t(c)])) > 8)
                        return false;
            return true;
        }

        video::CapturedFrame Wait(std::shared_future<video::CapturedFrame> frame)
        {
            // The presenter fills it with the next frame it draws; a window
            // that draws nothing (minimized) gets none.
            if (frame.valid() && frame.wait_for(std::chrono::seconds(3)) == std::future_status::ready)
            {
                try
                {
                    return frame.get();
                }
                catch (const std::future_error&)
                {
                }
            }
            return {};
        }

        struct Item
        {
            std::string name;
            std::string data;
            time_t modified;
        };

        // Files in the log folder, newest first.
        std::vector<std::filesystem::path> LogFiles(const char* prefix, const char* suffix)
        {
            std::vector<std::filesystem::path> files;
            std::error_code ec;
            for (std::filesystem::directory_iterator it(LogDirectory(), ec), end; !ec && it != end; it.increment(ec))
            {
                std::string name = it->path().filename().string();
                if (!name.empty() && name[0] != '.' && name.starts_with(prefix) && name.ends_with(suffix) && it->is_regular_file(ec))
                    files.push_back(it->path());
            }
            std::sort(files.rbegin(), files.rend());
            return files;
        }

        void Save(std::shared_future<video::CapturedFrame> frame, bool menuFrame, std::string stamp)
        {
            SetHostThreadName("nfsmw-bugreport");
            auto started = std::chrono::steady_clock::now();
            video::CapturedFrame shot = Wait(frame);
            // The menu opened on a blank frame (mid-fade): the game under the
            // menu now shows more than nothing.
            const char* shotNote = "screenshot.png  the game as it was when the menu opened";
            if (menuFrame && Blank(shot))
                if (video::CapturedFrame now = Wait(video::CaptureFrame().share()); !Blank(now))
                {
                    shot = std::move(now);
                    shotNote = "screenshot.png  the game under the menu (it was blank as the menu opened)";
                }
            FlushLog();
            fprintf(stderr, "[bugreport] saving (screenshot %ux%u)\n", shot.width, shot.height);
            time_t now = time(nullptr);

            std::vector<Item> items;
            std::vector<std::string> notes;  // for README.txt
            items.push_back({ "system.txt", SystemSummary(), now });
            notes.push_back("system.txt      this computer, the game install, and the settings in effect now");

            std::filesystem::path settingsPath = settings::DefaultPath();
            std::string settingsText;
            if (ReadWhole(settingsPath, settingsText))
            {
                items.push_back({ "settings.toml", settingsText, ModifiedTime(settingsPath) });
                notes.push_back("settings.toml   the saved settings");
            }
            else
                notes.push_back("(no settings.toml: every setting is at its default)");

            std::vector<uint8_t> png = Screenshot(shot);
            if (!png.empty())
                notes.push_back(shotNote);
            else
                notes.push_back("(no screenshot: the game showed no frame to capture)");

            std::vector<std::filesystem::path> logs = LogFiles("", ".log");
            for (const auto& path : logs)
            {
                std::string text;
                if (ReadWhole(path, text))
                    items.push_back({ "logs/" + path.filename().string(), std::move(text), ModifiedTime(path) });
            }
            notes.push_back(std::format("logs/           the last {} sessions' logs ({}.log is this one)", logs.size(), SessionName()));
            if (detail::LogCapped())
            {
                // The file stopped at its cap; the latest lines are in memory.
                items.push_back({ std::format("logs/{}-latest.txt", SessionName()), RecentOutput(2000), now });
                notes.push_back(std::format("                ({}.log reached its size cap: {}-latest.txt has the last lines)",
                    SessionName(), SessionName()));
            }
            size_t reports[2] = {};
            for (int kind = 0; kind < 2; kind++)
                for (const auto& path : LogFiles(kind == 0 ? "crash-" : "hang-", ".txt"))
                {
                    std::string text;
                    if (ReadWhole(path, text))
                    {
                        items.push_back({ "reports/" + path.filename().string(), std::move(text), ModifiedTime(path) });
                        reports[kind]++;
                    }
                }
            notes.push_back(reports[0] + reports[1]
                    ? std::format("reports/        what the game wrote when it crashed ({}) or hung ({})", reports[0], reports[1])
                    : "(no crash or hang reports)");

            std::string readme = std::format(
                "armb02 (Black Ops II, Xbox 360): bug report\n"
                "Saved {} (session {}, {:.0f} s in)\n"
                "Build: armb02 {}\n\n"
                "In here:\n",
                stamp, SessionName(), SessionSeconds(), BuildString());
            for (const std::string& note : notes)
                readme += "  " + note + "\n";
            readme += "\nPaths in your home folder are shown as ~. Nothing was sent anywhere: send this file\n"
                      "with a few words on what you were doing and what went wrong.\n";

            ZipWriter zip;
            bool ok = true;
            auto addText = [&](const std::string& name, const std::string& text, time_t modified) {
                std::string scrubbed = ScrubHome(text);
                ok &= zip.Add(name, scrubbed.data(), scrubbed.size(), modified);
            };
            addText("README.txt", readme, now);
            for (const Item& item : items)
                addText(item.name, item.data, item.modified);
            if (!png.empty())
                ok &= zip.Add("screenshot.png", png.data(), png.size(), now);
            const std::vector<uint8_t>& archive = zip.Finish();

            // Through a .partial file and a rename; "" when saved, else why not.
            auto saveTo = [&](const std::filesystem::path& folder, const std::filesystem::path& path) -> std::string {
                if (!ok)
                    return "the archive couldn't be built";
                std::filesystem::path partial = path;
                partial += ".partial";
                std::error_code ec;
                std::filesystem::create_directories(folder, ec);
                // write(2) rather than a stream, for an errno that says why.
                int fd = ::open(partial.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
                if (fd < 0)
                    return strerror(errno);
                int error = 0;
                for (size_t done = 0; done < archive.size() && !error;)
                {
                    ssize_t n = ::write(fd, archive.data() + done, archive.size() - done);
                    if (n > 0)
                        done += size_t(n);
                    else if (n == 0 || errno != EINTR)
                        error = n == 0 ? ENOSPC : errno;
                }
                if (::close(fd) != 0 && !error)
                    error = errno;
                if (!error && ::rename(partial.c_str(), path.c_str()) != 0)
                    error = errno;
                if (error)
                {
                    std::filesystem::remove(partial, ec);
                    return strerror(error);
                }
                return {};
            };
            std::filesystem::path folder = OutputFolder();
            std::filesystem::path path = folder / std::format("speedbreaker-bug-report-v{}-{}.zip", Version(), stamp);
            std::string failed = saveTo(folder, path);
            // The Desktop refused it (full, or macOS's privacy prompt said
            // no): the data folder, where the logs are, is ours.
            if (!failed.empty() && !std::getenv("NFSMW_BUG_REPORT_DIR") && folder != GetUserPath())
            {
                fprintf(stderr, "[bugreport] couldn't save in %s (%s); trying the data folder\n", folder.c_str(), failed.c_str());
                std::filesystem::path fallback = GetUserPath();
                std::filesystem::path fallbackPath = fallback / path.filename();
                if (std::string again = saveTo(fallback, fallbackPath); again.empty())
                {
                    folder = fallback;
                    path = fallbackPath;
                    failed.clear();
                }
            }
            if (!failed.empty())
            {
                std::string why = std::format("Couldn't save the bug report in {}: {}", ScrubHome(folder.string()), failed);
                fprintf(stderr, "[bugreport] %s\n", why.c_str());
                SetMessage(why);
                s_state.store(BugReportState::Failed);
                ui::Toast(why, 12.0);
                return;
            }
            double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
            std::string size = archive.size() < (1 << 20) ? std::format("{} KB", (archive.size() + 1023) >> 10)
                                                          : std::format("{:.1f} MB", double(archive.size()) / (1 << 20));
            fprintf(stderr, "[bugreport] saved %s (%zu files, %s, %.1f s)\n", path.c_str(), zip.Entries(), size.c_str(), seconds);
            std::string where = Describe(path);
            SetMessage("Saved to " + where);
            s_state.store(BugReportState::Saved);
            ui::Toast(std::format("Bug report saved: {} ({}). Send it along with what happened.", where, size), 15.0);
        }
    }

    void SaveBugReport()
    {
        if (s_state.load() == BugReportState::Saving)
            return;
        s_state.store(BugReportState::Saving);
        SetMessage("Saving...");
        fprintf(stderr, "[bugreport] Save Bug Report\n");
        bool menuFrame = s_menuFrame.valid();
        std::shared_future<video::CapturedFrame> frame = menuFrame ? s_menuFrame : video::CaptureFrame().share();
        std::thread(Save, std::move(frame), menuFrame, detail::TimeStamp()).detach();
    }

    void NoteMenuOpened()
    {
        s_menuFrame = video::CaptureFrame().share();
    }

    void NoteMenuClosed()
    {
        s_menuFrame = {};
    }

    BugReportState GetBugReportState()
    {
        return s_state.load();
    }

    std::string BugReportMessage()
    {
        std::lock_guard lock(s_messageMutex);
        return s_message;
    }

    std::string ScrubHome(std::string text)
    {
        std::vector<std::string> homes;
        if (const char* h = std::getenv("HOME"); h && strlen(h) > 1)
        {
            std::string home = h;
            while (home.size() > 1 && home.back() == '/')
                home.pop_back();
            homes.push_back(home);
            std::error_code ec;
            std::string real = std::filesystem::weakly_canonical(home, ec).string();
            while (real.size() > 1 && real.back() == '/')
                real.pop_back();
            if (!ec && real.size() > 1 && real != home)
                homes.push_back(real);
        }
        std::sort(homes.begin(), homes.end(), [](const std::string& a, const std::string& b) { return a.size() > b.size(); });
        auto pathChar = [](char c) { return std::isalnum(uint8_t(c)) || c == '.' || c == '_' || c == '-'; };
        for (const std::string& home : homes)
        {
            std::string out;
            out.reserve(text.size());
            size_t from = 0;
            for (size_t at; (at = text.find(home, from)) != std::string::npos;)
            {
                size_t end = at + home.size();
                // Only the whole folder (/home/deck, not /home/deck2) and not
                // the tail of a longer path (/mnt/home/deck).
                bool whole = (end == text.size() || !pathChar(text[end])) && (at == 0 || !pathChar(text[at - 1]));
                out.append(text, from, at - from);
                out += whole ? "~" : home;
                from = end;
            }
            out.append(text, from, std::string::npos);
            text.swap(out);
        }
        return text;
    }

    // deadpool-decomp: DP_SCREENSHOTS=<seconds> saves the game's frame every that many seconds
    // as logs/shots/<session>-<n>.png (a bring-up aid: see what the game shows without watching).
    void StartPeriodicScreenshots()
    {
        const char* v = std::getenv("DP_SCREENSHOTS");
        const int every = v ? std::atoi(v) : 0;
        if (every <= 0)
            return;
        std::thread([every] {
            std::filesystem::path dir = LogDirectory() / "shots";
            std::error_code ec;
            std::filesystem::create_directories(dir, ec);
            std::string stem = SessionLogPath().stem().string();
            for (int n = 1;; n++)
            {
                std::this_thread::sleep_for(std::chrono::seconds(every));
                video::CapturedFrame frame = Wait(video::CaptureFrame().share());
                std::vector<uint8_t> png = Screenshot(frame);
                if (png.empty())
                    continue;
                std::filesystem::path path = dir / std::format("{}-{:03}.png", stem, n);
                if (FILE* f = fopen(path.string().c_str(), "wb"))
                {
                    fwrite(png.data(), 1, png.size(), f);
                    fclose(f);
                    fprintf(stderr, "[shot] %s (%ux%u)\n", path.string().c_str(), frame.width, frame.height);
                }
            }
        }).detach();
    }

    namespace
    {
        // deadpool-decomp: the flight recorder's screenshots: one every 250 ms, ~640 wide, PNG, the
        // last 10 s kept (DP_RECORDER=0: none).
        struct RecorderShot { double seconds; std::vector<uint8_t> png; };
        std::mutex s_recorderShotsMutex;
        std::deque<RecorderShot> s_recorderShots;
    }

    void StartFlightRecorder()
    {
        const char* v = std::getenv("DP_RECORDER");
        if (v && v[0] == '0')
            return;
        std::thread([] {
            for (;;)
            {
                std::this_thread::sleep_for(std::chrono::milliseconds(250));
                video::CapturedFrame f = Wait(video::CaptureFrame().share());
                if (f.rgb.empty() || f.width < 2)
                    continue;
                const uint32_t k = std::max(1u, f.width / 640);
                const uint32_t w = f.width / k, h = f.height / k;
                std::vector<uint8_t> px(size_t(w) * h * 3);
                for (uint32_t y = 0; y < h; y++)
                    for (uint32_t x = 0; x < w; x++)
                        for (int c = 0; c < 3; c++)
                        {
                            uint32_t sum = 0;
                            for (uint32_t dy = 0; dy < k; dy++)
                                for (uint32_t dx = 0; dx < k; dx++)
                                    sum += f.rgb[(size_t(y * k + dy) * f.width + x * k + dx) * 3 + size_t(c)];
                            px[(size_t(y) * w + x) * 3 + size_t(c)] = uint8_t(sum / (k * k));
                        }
                RecorderShot shot{ SessionSeconds(), EncodePng(px.data(), w, h) };
                std::lock_guard lock(s_recorderShotsMutex);
                s_recorderShots.push_back(std::move(shot));
                while (!s_recorderShots.empty() && s_recorderShots.back().seconds - s_recorderShots.front().seconds > 10.0)
                    s_recorderShots.pop_front();
            }
        }).detach();
    }

    void SaveFlightRecorder()
    {
        static std::atomic<int> saves{ 0 };
        const int n = ++saves;
        std::vector<RecorderShot> shots;
        {
            std::lock_guard lock(s_recorderShotsMutex);
            shots.assign(s_recorderShots.begin(), s_recorderShots.end());
        }
        std::thread([n, shots = std::move(shots)] {
            std::filesystem::path dir = LogDirectory() / "recordings" / std::format("{}-{}", SessionLogPath().stem().string(), n);
            std::error_code ec;
            std::filesystem::create_directories(dir, ec);
            for (const RecorderShot& shot : shots)
                if (FILE* f = fopen((dir / std::format("{:08.2f}s.png", shot.seconds)).string().c_str(), "wb"))
                {
                    fwrite(shot.png.data(), 1, shot.png.size(), f);
                    fclose(f);
                }
            fprintf(stderr, "[recorder] F9 #%d: %zu screenshots to %s\n", n, shots.size(), dir.string().c_str());
        }).detach();
    }

    void SaveMarkedScreenshot()
    {
        static std::atomic<int> marks{ 0 };
        const int n = ++marks;
        std::thread([n] {
            video::CapturedFrame frame = Wait(video::CaptureFrame().share());
            std::vector<uint8_t> png = Screenshot(frame);
            if (png.empty())
                return;
            std::filesystem::path dir = LogDirectory() / "marks";
            std::error_code ec;
            std::filesystem::create_directories(dir, ec);
            std::filesystem::path path = dir / std::format("{}-{:03}.png", SessionLogPath().stem().string(), n);
            if (FILE* f = fopen(path.string().c_str(), "wb"))
            {
                fwrite(png.data(), 1, png.size(), f);
                fclose(f);
                fprintf(stderr, "[mark] %d: %s\n", n, path.string().c_str());
            }
        }).detach();
    }

    void StartShaderSweep()
    {
        static std::atomic<bool> running{ false };
        if (running.exchange(true))
            return;
        static int sweeps = 0;
        const int n = ++sweeps;
        std::thread([n] {
            // Shrink to 640 wide (box filter) so a sweep of ~100 frames stays small.
            auto shrink = [](const video::CapturedFrame& f, uint32_t& w, uint32_t& h) {
                const uint32_t k = std::max(1u, f.width / 640);
                w = f.width / k;
                h = f.height / k;
                std::vector<uint8_t> out(size_t(w) * h * 3);
                for (uint32_t y = 0; y < h; y++)
                    for (uint32_t x = 0; x < w; x++)
                        for (int c = 0; c < 3; c++)
                        {
                            uint32_t sum = 0;
                            for (uint32_t dy = 0; dy < k; dy++)
                                for (uint32_t dx = 0; dx < k; dx++)
                                    sum += f.rgb[(size_t(y * k + dy) * f.width + x * k + dx) * 3 + size_t(c)];
                            out[(size_t(y) * w + x) * 3 + size_t(c)] = uint8_t(sum / (k * k));
                        }
                return out;
            };
            auto save = [](const std::filesystem::path& path, const std::vector<uint8_t>& px, uint32_t w, uint32_t h) {
                std::vector<uint8_t> png = EncodePng(px.data(), w, h);
                if (FILE* f = fopen(path.string().c_str(), "wb"))
                {
                    fwrite(png.data(), 1, png.size(), f);
                    fclose(f);
                }
            };
            auto settle = [] { std::this_thread::sleep_for(std::chrono::milliseconds(120)); };
            std::filesystem::path dir = LogDirectory() / "sweep" / std::format("{}-{}", SessionLogPath().stem().string(), n);
            std::error_code ec;
            std::filesystem::create_directories(dir, ec);
            gpu::renderer::SetSkipPixelShader(0);
            gpu::renderer::StartCollectingPixelShaders();
            std::this_thread::sleep_for(std::chrono::milliseconds(300));
            std::vector<uint64_t> shaders = gpu::renderer::TakeCollectedPixelShaders();
            fprintf(stderr, "[sweep] %s: %zu pixel shaders\n", dir.string().c_str(), shaders.size());
            uint32_t w = 0, h = 0;
            video::CapturedFrame first = Wait(video::CaptureFrame().share());
            if (first.rgb.empty())
            {
                running = false;
                return;
            }
            std::vector<uint8_t> base = shrink(first, w, h);
            save(dir / "base.png", base, w, h);
            uint32_t saved = 0;
            for (uint64_t hash : shaders)
            {
                gpu::renderer::SetSkipPixelShader(hash);
                settle();
                video::CapturedFrame f = Wait(video::CaptureFrame().share());
                gpu::renderer::SetSkipPixelShader(0);
                if (f.rgb.empty())
                    continue;
                uint32_t fw = 0, fh = 0;
                std::vector<uint8_t> px = shrink(f, fw, fh);
                if (fw != w || fh != h)
                    continue;
                uint64_t changed = 0;
                for (size_t i = 0; i + 2 < px.size(); i += 3)
                    changed += std::abs(int(px[i]) - int(base[i])) + std::abs(int(px[i + 1]) - int(base[i + 1])) +
                        std::abs(int(px[i + 2]) - int(base[i + 2])) > 48;
                fprintf(stderr, "[sweep] ps_%016llx: %llu of %u pixels change\n", (unsigned long long)hash,
                    (unsigned long long)changed, w * h);
                if (changed > uint64_t(w) * h / 2000)
                {
                    save(dir / std::format("ps_{:016x}.png", hash), px, w, h);
                    saved++;
                }
                settle();
            }
            fprintf(stderr, "[sweep] done: %u of %zu shaders change the frame\n", saved, shaders.size());
            running = false;
        }).detach();
    }

    // deadpool-decomp: DP_DUMP_GUEST=<seconds>@<hex addr>:<bytes>,<hex addr>:<bytes>,... saves those
    // guest memory ranges once, at that time, as logs/dumps/<session>-<addr>.bin (inspecting resolved
    // textures offline: tools/decode_xenos_texture.py).
    void StartGuestDumps()
    {
        const char* v = std::getenv("DP_DUMP_GUEST");
        if (!v)
            return;
        std::string spec = v;
        std::thread([spec] {
            double seconds = std::atof(spec.c_str());
            std::this_thread::sleep_for(std::chrono::duration<double>(seconds));
            std::filesystem::path dir = LogDirectory() / "dumps";
            std::error_code ec;
            std::filesystem::create_directories(dir, ec);
            std::string stem = SessionLogPath().stem().string();
            size_t at = spec.find('@');
            std::string list = at == std::string::npos ? "" : spec.substr(at + 1);
            for (size_t pos = 0; pos < list.size();)
            {
                size_t end = list.find(',', pos);
                std::string item = list.substr(pos, end == std::string::npos ? std::string::npos : end - pos);
                pos = end == std::string::npos ? list.size() : end + 1;
                uint32_t addr = 0, bytes = 0;
                if (sscanf(item.c_str(), "%x:%u", &addr, &bytes) != 2 || bytes == 0)
                    continue;
                std::filesystem::path path = dir / std::format("{}-{:08X}.bin", stem, addr);
                if (FILE* f = fopen(path.string().c_str(), "wb"))
                {
                    fwrite(g_memory.Translate(addr), 1, bytes, f);
                    fclose(f);
                    fprintf(stderr, "[dump] %s (%u bytes)\n", path.string().c_str(), bytes);
                }
            }
        }).detach();
    }
}
