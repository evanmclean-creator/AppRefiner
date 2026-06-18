// Vim mode support for AppRefiner.
// Portions of the Vim behavior are derived from NppVim
// (https://github.com/h-jangra/NppVim), licensed under the MIT License.

#pragma once

#include "../Common.h"
#include <string>

enum class VimMode {
    Normal,
    Insert,
    Visual
};

// Forward declaration for trie node (defined in VimKeymap.h).
struct VimKeymapNode;

enum class VimOpType {
    None,
    DeleteLine,   // dd
    YankLine,     // yy
    Paste,        // p / P
    MotionOp,     // op + motion  (dw, d$, yG, etc.)
    TextObjectOp, // op + text-object  (diw, ci", da(, etc.)
    Replace,      // r<char>
};

struct VimLastOp {
    VimOpType type     = VimOpType::None;
    int       count    = 0;
    char      op       = 0;
    char      motion   = 0;   // for MotionOp
    char      textMod  = 0;   // 'i' or 'a' for TextObjectOp
    char      textObj  = 0;   // 'w', '(', '"', etc. for TextObjectOp
    char      replChar = 0;   // for Replace
};

struct VimEditorState {
    VimMode mode             = VimMode::Normal;
    bool    caretInitialized = false;
    int     pendingCount     = 0;
    VimKeymapNode* keymapCurrentNode = nullptr; // non-owning trie cursor
    std::string    keymapPendingKeys;
    int     visualAnchor     = -1;
    int     visualCaret      = -1;
    bool    visualLinewise   = false;
    char    visualTextObjectPending = 0;

    // Operator-pending state machine
    char opPending         = 0;
    char textObjectPending = 0;
    bool replacePending    = false;
    VimLastOp lastOp;

    // Find-char (f/F/t/T) pending state
    char findCharOp   = 0;   // 'f','F','t','T' while waiting for the target char, else 0
    char lastFindOp   = 0;   // last op used, for ; and , repeat
    char lastFindChar = 0;   // last target char, for ; and , repeat

    // Search prompt state
    bool        searchPending = false;
    bool        searchForward = true;
    std::string searchText;

    // Marks: a-z, document byte position, -1 = not set
    int  marks[26];
    int  jumpMarkPos = -1;   // position before last mark/search jump (for '' and ``)

    // 2-char pending states
    bool markSetPending    = false;  // 'm' waiting for letter
    bool jumpExactPending  = false;  // '`' waiting for letter
    bool jumpLinePending   = false;  // '\'' waiting for letter

    // Register prefix ('"' was pressed, waiting for letter)
    char activeRegister    = 0;      // 0 = default; 'a'-'z' = named register
    bool registerPending   = false;

    // Macro
    bool macroRecordPending = false; // 'q' pressed (not recording), waiting for letter
    bool macroPlayPending   = false; // '@' pressed, waiting for letter

    // Colon command prompt
    bool        colonPending = false;
    std::string colonBuffer;

    VimEditorState() {
        for (int i = 0; i < 26; i++) marks[i] = -1;
    }
};

// Named register entry — shared between yank, delete, and macro storage.
struct RegisterEntry {
    std::string text;
    bool        linewise = false;
};

struct VimSessionState {
    std::string yankRegister;
    bool        yankLinewise = false;

    // Named registers a-z
    RegisterEntry namedRegisters[26];

    // Macro recording/playback
    bool        macroRecording  = false;
    char        macroRecordReg  = 0;
    std::string macroRecordBuf;
    char        lastMacroReg    = 0;
    int         macroPlayDepth  = 0;  // recursion guard for @@ / nested macros

    // Ex-command options (persist across editors in the same session)
    bool ignoreCaseEnabled = false;
};

VimEditorState& GetVimEditorState(HWND hwnd);
VimSessionState& GetVimSessionState();
void RemoveVimEditorState(HWND hwnd);
void SetVimEnabledForKnownEditors(bool enabled);
void EnsureVimCaretInitialized(HWND hwnd, VimEditorState& state);
bool HandleVimEditorMessage(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam, VimEditorState& state);
void EnterVimInsertMode(HWND hwnd, VimEditorState& state);
void ExitVimInsertMode(HWND hwnd, VimEditorState& state);
// Returns true only if hwnd is a known, initialized Vim editor in Normal mode.
// Safe to call with any HWND — does not create a new state entry.
bool IsVimNormalModeEditor(HWND hwnd);
