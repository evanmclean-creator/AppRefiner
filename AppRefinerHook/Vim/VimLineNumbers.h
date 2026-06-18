// Hybrid line numbers for Vim mode: current line shows absolute (1-based),
// all other visible lines show relative distance from the cursor.

#pragma once

#include "../Common.h"

namespace VimLineNumbers {
    // Call when Vim mode is enabled for an editor (or on first subclass if already enabled).
    void Init(HWND hwnd);

    // Call when Vim mode is disabled for an editor (restores default margin).
    void Shutdown(HWND hwnd);

    // Call on SCN_UPDATEUI (SC_UPDATE_SELECTION | SC_UPDATE_V_SCROLL) to refresh numbers.
    void Update(HWND hwnd);
}
