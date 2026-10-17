#pragma once

#include <SDL.h>

#include <string>
#include <vector>

namespace plumsg {

enum class DialogResult { Chosen, Cancelled, Unavailable };

// The system "open file" dialog, filtered to Mega Drive ROMs. Blocks until closed.
// Returns Unavailable on platforms without one; the caller then uses FileBrowser.
DialogResult openRomDialog(SDL_Window* window, const std::string& startFolder, std::string* chosen);

// Minimal file picker drawn with Dear ImGui, for platforms without a native dialog.
class FileBrowser {
public:
    void open(const std::string& folder);
    // Call every frame between ImGui::NewFrame and ImGui::Render. Returns true once a
    // file has been picked, with its path in *chosen.
    bool draw(std::string* chosen);

private:
    struct Entry {
        std::string name;
        bool folder;
    };
    void list(const std::string& folder);

    bool open_ = false;
    bool justOpened_ = false;
    std::string folder_;
    std::vector<Entry> entries_;
};

}  // namespace plumsg
