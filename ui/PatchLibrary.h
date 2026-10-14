// File > Patch Library... (Cmd/Ctrl-B, the original's Patch Browser key): the patches and performances in folders
// of the user's own, to find, hear and open. A G2fresh addition around the original's Patch Browser Disk tab
// (CHardDriveView, manual p111), which lists one folder level by file name.
//
// As the original: a click selects, a double-click or Return opens the file as File > Open does, Up and Down move
// the selection, a right-click offers the slots (File > Open To). G2fresh's own: the folders are indexed with their
// subfolders (g2/library.hpp, on a background thread, cached beside G2fresh's settings), the list has columns, a
// search and filters, a detail pane shows what a file holds, files can be starred, and Space auditions the selected
// file on the connected synth without touching the document (SynthSync::audition): it plays until Space again,
// another audition, an Open, or the window closing, which puts back what was there. The original's Save In Synth is
// left out: nothing here writes to the synth's memory, and the library's files are only ever read.
#pragma once

#include "PatchDocument.h"
#include "SynthSync.h"

#include <juce_gui_basics/juce_gui_basics.h>

#include <functional>
#include <memory>

namespace g2ui {

// What the library needs of the editor it belongs to.
struct LibraryHost {
    PatchDocument& doc;
    SynthSync* synth = nullptr; // nullptr: no synth to audition on
    // Asks before the document is replaced, then calls back (MainView::confirmDiscard).
    std::function<void(std::function<void()>)> confirmDiscard;
    // Opens a file as File > Open does (MainView::openFile).
    std::function<void(const juce::File&)> openFile;
};

// Where the library keeps its folders, stars and recent files (keys "libraryFolders", "libraryFavourites",
// "libraryRecent": one path per line) and its index: G2fresh's settings and a file beside them, or, for g2render,
// somewhere that is not the user's.
struct LibraryStore {
    std::function<juce::String(const juce::String& key)> get;
    std::function<void(const juce::String& key, const juce::String& value)> set;
    juce::File cacheFile;

    static LibraryStore userSettings();
};

class PatchLibrary : public juce::DocumentWindow {
public:
    explicit PatchLibrary(LibraryHost host, LibraryStore store = LibraryStore::userSettings());
    ~PatchLibrary() override;

    // Shows the window, reads the folders again and puts the keyboard in the search field.
    void open();
    // Closing ends an audition.
    void closeButtonPressed() override;

private:
    std::unique_ptr<class PatchLibraryPanel> panel_;
};

// The window's content alone, reading its folders (g2render draws it); libraryScanning() says whether it still does.
std::unique_ptr<juce::Component> makePatchLibraryPanel(LibraryHost host, LibraryStore store);
bool libraryScanning(juce::Component& panel);
// Types `words` in the search field, and selects the first file whose name contains `text` (g2render: a list and
// a detail pane to look at).
void librarySearch(juce::Component& panel, const juce::String& words);
void librarySelect(juce::Component& panel, const juce::String& text);

} // namespace g2ui
