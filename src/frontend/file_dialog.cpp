#include "file_dialog.h"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <iterator>
#include <system_error>

#include "imgui.h"

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <commdlg.h>
#include <SDL_syswm.h>
#endif

namespace plumsg {

namespace fs = std::filesystem;

namespace {

bool isRom(const fs::path& p) {
    std::string ext = p.extension().string();
    std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return ext == ".md" || ext == ".gen" || ext == ".smd" || ext == ".bin" || ext == ".zip";
}

#ifdef _WIN32
std::wstring widen(const std::string& s) {
    if (s.empty()) return {};
    int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, nullptr, 0);
    std::wstring w(n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, w.data(), n);
    w.resize(n - 1);
    return w;
}

std::string narrow(const wchar_t* w) {
    int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, nullptr, 0, nullptr, nullptr);
    std::string s(n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w, -1, s.data(), n, nullptr, nullptr);
    s.resize(n - 1);
    return s;
}
#endif

}  // namespace

DialogResult openRomDialog(SDL_Window* window, const std::string& startFolder, std::string* chosen) {
#ifdef _WIN32
    HWND owner = nullptr;
    SDL_SysWMinfo info;
    SDL_VERSION(&info.version);
    if (SDL_GetWindowWMInfo(window, &info)) owner = info.info.win.window;

    wchar_t file[MAX_PATH * 4] = L"";
    const std::wstring folder = widen(startFolder);
    OPENFILENAMEW ofn{};
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = owner;
    ofn.lpstrFilter = L"ROMs de Mega Drive (*.md, *.gen, *.smd, *.bin, *.zip)\0*.md;*.gen;*.smd;*.bin;*.zip\0Todos los archivos\0*.*\0";
    ofn.lpstrFile = file;
    ofn.nMaxFile = static_cast<DWORD>(std::size(file));
    ofn.lpstrInitialDir = folder.empty() ? nullptr : folder.c_str();
    ofn.lpstrTitle = L"Abrir ROM";
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
    if (!GetOpenFileNameW(&ofn)) return DialogResult::Cancelled;
    *chosen = narrow(file);
    return DialogResult::Chosen;
#else
    (void)window;
    (void)startFolder;
    (void)chosen;
    return DialogResult::Unavailable;
#endif
}

void FileBrowser::open(const std::string& folder) {
    std::error_code ec;
    fs::path start = folder.empty() ? fs::current_path(ec) : fs::path(folder);
    if (!fs::is_directory(start, ec)) start = fs::current_path(ec);
    list(start.string());
    open_ = true;
    justOpened_ = true;
}

void FileBrowser::list(const std::string& folder) {
    folder_ = folder;
    entries_.clear();
    std::error_code ec;
    for (fs::directory_iterator it(folder, fs::directory_options::skip_permission_denied, ec), end; !ec && it != end;
         it.increment(ec)) {
        const bool dir = it->is_directory(ec);
        if (dir || isRom(it->path())) entries_.push_back({it->path().filename().string(), dir});
    }
    std::sort(entries_.begin(), entries_.end(), [](const Entry& a, const Entry& b) {
        return a.folder != b.folder ? a.folder : a.name < b.name;
    });
}

bool FileBrowser::draw(std::string* chosen) {
    if (justOpened_) {
        ImGui::OpenPopup("Abrir ROM");
        justOpened_ = false;
    }
    bool picked = false;
    ImGui::SetNextWindowSize(ImVec2(520, 420), ImGuiCond_Appearing);
    if (ImGui::BeginPopupModal("Abrir ROM", &open_)) {
        ImGui::TextWrapped("%s", folder_.c_str());
        if (ImGui::Button("Subir")) list(fs::path(folder_).parent_path().string());
        ImGui::Separator();
        std::string next;
        if (ImGui::BeginChild("archivos", ImVec2(0, -ImGui::GetFrameHeightWithSpacing()))) {
            for (const auto& e : entries_) {
                const std::string label = e.folder ? "[" + e.name + "]" : e.name;
                if (ImGui::Selectable(label.c_str())) {
                    next = (fs::path(folder_) / e.name).string();
                    if (!e.folder) {
                        *chosen = next;
                        picked = true;
                        next.clear();
                    }
                }
            }
            if (entries_.empty()) ImGui::TextDisabled("No hay ROMs en esta carpeta.");
        }
        ImGui::EndChild();
        if (!next.empty()) list(next);
        if (ImGui::Button("Cancelar")) open_ = false;
        if (picked) open_ = false;
        if (!open_) ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }
    return picked;
}

}  // namespace plumsg
