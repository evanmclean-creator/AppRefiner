// Vim ':' ex-command layer for AppRefiner.
// Portions of the logic are derived from NppVim
// (https://github.com/h-jangra/NppVim), licensed under the MIT License.

#pragma once

#include "../Common.h"
#include <string>

struct VimEditorState;
struct VimSessionState;

// Execute a colon command.  cmd is the text typed after the leading ':'.
void ExecuteColonCommand(HWND hwnd, VimEditorState& state, VimSessionState& session,
                         const std::string& cmd);
