// Trie-based multi-key sequence matcher for Vim Normal mode.
// Ported from NppVim (https://github.com/h-jangra/NppVim), MIT License.
// Adapted to use per-editor state rather than a global VimState&.

#pragma once
#include "../Common.h"
#include <functional>
#include <string>
#include <unordered_map>
#include <memory>

struct VimEditorState; // defined in VimMode.h

using VimKeyHandler = std::function<void(HWND, int)>;

struct VimKeymapNode {
    VimKeyHandler handler;
    std::unordered_map<char, std::shared_ptr<VimKeymapNode>> children;
    bool isLeaf = false;
    char motionChar = 0;
};

class VimKeymap {
public:
    VimKeymap();

    // Register a key sequence with a handler.
    VimKeymap& set(const std::string& keys, VimKeyHandler handler);

    // Process one WM_CHAR key in Normal mode.
    // Handles digit count-prefix accumulation and trie traversal.
    // Returns true if key was consumed (digit accumulated, sequence pending, or command fired).
    // Returns false if key is not handled by the keymap; in that case state.pendingCount
    // is NOT reset, so the caller's switch statement can use it via ConsumeCount().
    bool handleKey(HWND hwnd, char key, VimEditorState& state);

private:
    std::shared_ptr<VimKeymapNode> root;
    void insertKeySequence(const std::string& keys, VimKeyHandler handler, char motionChar = 0);
    bool processKey(HWND hwnd, char key, int count, VimEditorState& state);
};

// Shared Normal-mode keymap trie. Initialized once at startup; per-editor cursor state
// is stored in VimEditorState::keymapCurrentNode / keymapPendingKeys.
extern VimKeymap* g_normalKeymap;

void InitializeNormalModeKeymap();
void ShutdownNormalModeKeymap();
