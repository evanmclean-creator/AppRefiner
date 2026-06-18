# AppRefiner Vim Motions — Implementation Brief

## 1. Goal

Add Vim-style modal editing (Normal / Insert / Visual modes, motions, operators,
text objects) to AppRefiner's Scintilla editor integration inside PeopleSoft
Application Designer, by porting the relevant logic from the open-source
**NppVim** plugin (https://github.com/h-jangra/NppVim, MIT licensed) into
AppRefiner's existing native hook DLL (`AppRefinerHook`), rather than building
a Vim engine from scratch or building a separate standalone application.

This is a native-C++-first feature. The Vim engine should run entirely inside
`AppRefinerHook.dll`, in-process with the Scintilla control it's editing, with
no per-keystroke round-trip to the C# side for Vim grammar decisions. C#
involvement should be limited to one-time toggles (enable/disable Vim mode)
and UI integration AppRefiner already owns, such as `K` tooltips and Vim
search prompt/highlight updates.

**Default mode is Normal**, matching real Vim — every PeopleCode editor should
open in Normal mode, not Insert mode, when Vim mode is enabled.

Both AppRefiner and NppVim are MIT licensed — porting code from NppVim into
AppRefiner is permitted; keep NppVim's copyright notice somewhere in the
ported files (a comment header crediting the original repo is sufficient).

## 2. Why this approach (context for whoever implements this)

- AppRefiner already injects `AppRefinerHook.dll` into the Application
  Designer process via `SetWindowsHookEx`-based hooks (`SetHook` /
  `SetKeyboardHook` in `AppRefinerHook/HookManager.cpp`, called from C# in
  `AppRefiner/Events/EventHookInstaller.cs`). This is the same mechanism any
  standalone tool would need to build from scratch — reuse it.
- AppRefiner already subclasses every PeopleCode Scintilla editor window via
  `SetWindowSubclass(scintillaChild, ScintillaSubclassProc, SCINTILLA_SUBCLASS_ID, callbackWindow)`
  (`AppRefinerHook/HookManager.cpp:1158` and `:1179`). `ScintillaSubclassProc`
  (defined at `AppRefinerHook/HookManager.cpp:657`) already does selective
  keydown/keyup interception (e.g. Esc-to-dismiss-autocomplete at line 695,
  modifier-key shortcut forwarding at line 715) and falls through to
  `DefSubclassProc` for everything else. Vim mode should be added as new
  branches inside this existing function, not a parallel hook.
- NppVim (a real, maintained, MIT-licensed Notepad++ plugin, ~10,200 lines of
  C++) implements the full Vim grammar (motions, operators, text objects,
  visual mode, marks, registers, macros, a `:` command line) almost entirely
  against raw Scintilla messages. Of its ~10,200 lines, only ~61 call sites
  reference Notepad++-specific APIs (`nppData` / `NPPM_*`); the rest is pure
  Scintilla and is portable close to verbatim.
- NppVim's actual swallow-vs-passthrough mechanism is simple: while in
  Normal/Visual mode, `WM_CHAR` is unconditionally consumed (`return 0`, no
  call to the original window proc) and dispatched to its own per-mode
  handler; while in Insert mode, everything falls through except a small set
  of claimed Ctrl-chords. AppRefiner's existing `ScintillaSubclassProc`
  already follows the identical "check condition, `return 0`, else fall
  through to default proc" pattern — this is a known-good, already-proven
  pattern in this exact codebase, not something to invent.

## 3. Source material to hand to Claude Code / Codex

Provide BOTH of the following, cloned locally, as context:

1. The AppRefiner repo: https://github.com/Gideon-Taylor/AppRefiner
2. NppVim: https://github.com/h-jangra/NppVim

Point the agent specifically at these NppVim files as the primary porting
source (line counts as of the version reviewed for this brief). **Do not give
the agent a pre-digested field list for the state struct — instruct it to
read `include/NppVim.h` (the `VimState` struct, defined at line 86) directly
and split the relevant state into AppRefiner's editor-local/session-shared
state model described in §4 point 2.** The same applies generally: where this
brief references a file/function, the agent should read that file itself
rather than rely solely on this brief's summary.

| File | Lines | Notepad++-specific calls | Notes |
|---|---|---|---|
| `src/Motion.cpp` | 370 | 0 | Pure Scintilla cursor-move primitives (charLeft/Right, lineUp/Down, wordRight/Left, wordEnd, pageUp/Down, paragraph motions, document start/end, gotoLine). Port near-verbatim. |
| `src/TextObject.cpp` | 455 | 0 | `iw`/`aw`/`i"`/`a(`/etc. text-object resolution. Port near-verbatim. |
| `src/Keymap.cpp` (+ `include/Keymap.h`) | 170 + 68 | 0 | Generic trie-based multi-key sequence matcher (handles `gg`, `dd`, custom mappings). Port the trie/sequence design, but do not keep NppVim's global-keymap/global-`VimState` coupling; adapt it to AppRefiner's per-editor/session-shared context model (§4 point 2). |
| `src/NormalMode.cpp` | 2,285 | 6 (`gf`, block-comment toggle, tab-switching — see §5) | Operator-pending state machine (includes count-prefix handling, e.g. `3j`, `5dd`, `2w` — this is in scope, see §5), all single-key and `g`-prefixed Normal-mode commands. Port the state machine and the bulk of commands; strip/stub the 6 flagged call sites. |
| `src/VisualMode.cpp` | 1,801 | 1 (block-comment toggle) | Character/Line/Block visual selection + operators. Port wholesale, stub the one flagged call site. |
| `src/Marks.cpp` | 316 | 3 (cross-file mark jump — see §5) | `m`, `` ` ``, `'` mark set/jump. Port; restrict to single-file marks (no cross-file jump) for v1. |
| `src/Utils.cpp` | 861 | 2 (`getCurrentScintillaHandle`, `setStatus` — see §6) | Shared helpers: word-bounds, undo grouping, clipboard, case conversion. Port the pure helpers, but adapt status/search-highlight helpers through AppRefiner's existing UI/highlighter plumbing instead of direct Notepad++ status or raw indicator ownership. |
| `src/CommandMode.cpp` | 1,639 | 0 in the search/substitute path; several elsewhere (`:e`, `:w`, `:q`, file/buffer commands — see §5) | **Split file — three parts.** (1) `/`/`?` search-prompt + `performSearch`/`searchNext`/`searchPrevious` (lines 1077-1246): pure Scintilla, **already ported in step 6**. (2) `performSubstitution` (lines ~843-960): pure Scintilla, **port for step 10** (`:s` core). The `c` confirm-each path uses `Utils::getCharBlocking()` — a blocking `GetMessage()` loop — **skip that flag only**; everything else ports clean. (3) Everything else in `handleColonCommand` (`:sort`, `:sp`/`:vs`, `:e`, `:w`/`:q`, buffer nav, `.rc` parsing) is deferred or out of scope — see §5. |
| `src/NppVim.cpp` | 985 | the rest | Do NOT port this file directly — it's NppVim's entry point/hook-install/DLL-lifecycle code, which AppRefiner already has its own equivalent of. Use only as a reference for the dispatch *logic* inside `ScintillaHookProc` (see §4), not as code to transplant. |

> **Note on the split file above:** `CommandMode.cpp` contains three independent
> pieces. Search (`/`, `?`, `n`, `N`) was ported in step 6. `:s` substitution
> (`performSubstitution`) is pure Scintilla and is ported in step 10 — the only
> part to skip is the `c` confirm-each path which depends on a blocking message
> loop. Everything else in `handleColonCommand` is deferred or out of scope per §5.

Also load these AppRefiner files as the integration target / reference for
existing patterns to mirror:

- `AppRefinerHook/HookManager.cpp` — specifically `ScintillaSubclassProc`
  (line 657) and the existing Esc-handling (line 695) and modifier-shortcut
  forwarding (line 715) blocks immediately following it.
- `AppRefinerHook/Common.h` — the `ShortcutType` bitfield enum (`SHORTCUT_NONE`,
  `SHORTCUT_COMMAND_PALETTE`, `SHORTCUT_OPEN`, `SHORTCUT_SEARCH`,
  `SHORTCUT_LINE_SELECTION`, `SHORTCUT_ALL`, lines 82-88).
- `AppRefiner/Events/EventHookInstaller.cs` — existing pattern for one-shot
  C#→C++ toggle messages (e.g. `WM_TOGGLE_AUTO_PAIRING`); mirror this exact
  pattern for a new `WM_AR_TOGGLE_VIM` message.
- `AppRefiner/TooltipProviders/TooltipManager.cs` — `ShowTooltip(editor,
  position, lineNumber)` at line 441, currently only called from
  `SCN_DWELLSTART` handling in `AppRefiner/MainForm.cs` (line 2202). This is
  the function the `K` keybinding should call, feeding it the *caret*
  position instead of a mouse-dwell position.
- `AppRefiner/MainForm.cs` — `AppDesignerProcesses` dictionary (line 79,
  `Dictionary<uint, AppDesignerProcess>`), `activeAppDesigner`/`activeEditor`
  fields (lines 77-78), and the `BypassSmartOpen()` method (line 1721) as the
  reference pattern for programmatically foregrounding a specific Application
  Designer instance via `WinApi.SetForegroundWindow(mainWindowHandle)`.
- `AppRefiner/AppDesignerProcess.cs` — `DBName` property (line 109), useful
  for distinguishing instances when cycling.
- `AppRefiner/Services/SettingsService.cs` — `GeneralSettingsData` class and
  `LoadGeneralSettings`/`SaveGeneralSettings`/`SaveChanges` methods; the
  existing `AutoPair` field/checkbox/propagation chain (also visible in
  `AppRefiner/MainForm.cs` around lines 224, 898, 950) is the exact pattern
  to copy for a persisted `VimModeEnabled` setting — see §4 point 5. Also
  update `AppRefiner/Properties/Settings.settings` and the generated
  `AppRefiner/Properties/Settings.Designer.cs` entry, or
  `Properties.Settings.Default.VimModeEnabled` will not exist.
- `AppRefiner/ScintillaEditor.cs` and `AppRefiner/ScintillaManager.cs` —
  AppRefiner already owns Scintilla indicator allocation/highlighter state
  starting at indicator 0. Vim search highlights must use this existing
  machinery rather than directly claiming an indicator number in C++.
- `AppRefiner/Commands/BaseCommand.cs` and a real example,
  `AppRefiner/Commands/BuiltIn/BetterFindCommand.cs` — shows the actual
  shortcut-registration API (`registrar.TryRegisterShortcut(commandId,
  ModifierKeys.Control, Keys.J, this)`), the pattern any new `BaseCommand`
  (e.g. "Toggle Vim Mode", cross-instance switching) should follow.

## 4. Architecture decisions to lock in before writing code

Tell the agent these decisions explicitly — don't let it improvise them:

1. **Vim engine lives in C++, inside `AppRefinerHook.dll`.** New files, e.g.
   `AppRefinerHook/Vim/VimState.h`, `VimMotion.cpp`, `VimNormalMode.cpp`,
   `VimVisualMode.cpp`, `VimTextObject.cpp`, `VimKeymap.cpp`, `VimMarks.cpp` —
   mirroring NppVim's file split. No C# round-trip per keystroke.
2. **Hybrid state: editor-local mode state, session-shared registers/macros.**
   NppVim uses one global `VimState` struct because Notepad++ effectively has
   one or two visible editors. Application Designer can have many PeopleCode
   editors open simultaneously, each its own Scintilla `HWND`, all routed
   through the same `ScintillaSubclassProc`. Do not port NppVim's singleton
   `VimState state` as-is. Split it into:
   - `VimEditorState`, stored in
     `std::unordered_map<HWND, VimEditorState> g_vimEditorStates;`, keyed by
     the `hWnd` parameter `ScintillaSubclassProc` already receives. This owns
     editor-local state: mode, repeat/operator-pending state, visual
     selection anchor/type, caret style, local marks, current search
     highlights, and any prompt state tied to the active editor.
   - `VimSessionState`, one shared instance per injected
     `AppRefinerHook.dll`/Application Designer process. This owns state that
     should naturally survive editor switches inside the same Application
     Designer instance: registers, macro recording/playback, and last search
     term/direction. The clipboard register should continue to use the OS
     clipboard so copy/paste can cross editor/process boundaries.
   Look up (or default-construct, defaulting to Normal mode per §1) the
   editor's state at the top of the new dispatch branch, then pass both the
   editor state and the session state through the ported Vim handlers.
   **Important:** NppVim's `Keymap`, `NormalMode`, `VisualMode`, `CommandMode`,
   `Marks`, and many helpers reference global `state`/global keymaps. Refactor
   these to receive a current `VimContext`/state reference, or store the
   keymaps/mode handlers per editor, rather than keeping a single keymap
   object bound to one `VimState&`.
3. **Clean up state on window destroy.** `ScintillaSubclassProc` already
   calls `RemoveWindowSubclass` on `WM_NCDESTROY` (line 661). Add
   `g_vimEditorStates.erase(hWnd)` at that same point so closed PeopleCode
   editors don't leak dead state entries over a long session. If Vim search
   highlights are being displayed through AppRefiner's highlighter machinery,
   clear those editor-owned highlights as part of the same cleanup path.
4. **Integration point: new branches inside the existing
   `ScintillaSubclassProc`, ordered correctly relative to existing logic.**
   Specifically:
   - Vim interception must run AFTER the existing Esc/autocomplete-dismiss
     check (line 695) and the existing autocomplete-active /
     call-tip-active checks — if `SCI_AUTOCACTIVE` or `SCI_CALLTIPACTIVE` is
     true, let AppRefiner's existing logic handle the keystroke; don't let
     Vim mode steal it.
   - Do not consume `WM_KEYUP` in the Vim branch. AppRefiner currently uses
     key-up handling to forward modifier shortcuts to C#, and those existing
     shortcuts must keep working in Normal, Visual, and Insert mode.
   - When the per-editor `VimEditorState.mode` is `NORMAL` or `VISUAL`:
     consume `WM_CHAR` and dispatch it into the ported Normal/Visual mode
     handler, returning `0` so Scintilla never inserts the typed character.
     Handle only the `WM_KEYDOWN` keys/chords Vim actually claims (arrows,
     function-style specials, Escape, relevant Ctrl chords, etc.); leave
     unhandled keydown messages to the existing fall-through path.
   - When `VimEditorState.mode` is `INSERT` (or Vim mode is globally disabled
     for this editor): fall through to AppRefiner's existing logic exactly as
     today, except for a small set of Insert-mode Ctrl-chords NppVim defines
     (Ctrl-W delete word, Ctrl-U delete to line start, etc.) — these must be
     checked for collisions against AppRefiner's existing shortcut table
     (see §7) before being wired in.
5. **Persistent on/off setting, with both a settings checkbox AND a Command
   Palette toggle, sharing one source of truth.** Vim mode must remember
   your choice across AppRefiner restarts. AppRefiner already has the
   exact pattern to copy for this: `AutoPair` in `GeneralSettingsData`
   (`AppRefiner/Services/SettingsService.cs`), persisted via .NET's
   `Properties.Settings.Default` (writes to a `user.config` file under the
   user profile, survives restarts once `.Save()`/`SaveChanges()` is
   called), bound to a checkbox (`chkAutoPairing`) in the general settings
   dialog (`AppRefiner/MainForm.cs` lines ~224, 898, 950), and propagated to
   the native hook via a `NotifyAutoPairingChange`-style function whenever
   it changes (mirrors the existing `g_enableAutoPairing` global + a
   `WM_TOGGLE_AUTO_PAIRING`-style one-shot message). Concretely:
   - Add `VimModeEnabled` (bool) to `GeneralSettingsData`, loaded/saved
     alongside `AutoPair` and the other existing fields in
     `LoadGeneralSettings`/`SaveGeneralSettings`. Also add the setting to
     `AppRefiner/Properties/Settings.settings` and the generated
     `Settings.Designer.cs` file, or the typed setting access will fail.
   - Add a corresponding checkbox to the general settings dialog, wired the
     same way `chkAutoPairing` is.
   - Add a single function (e.g. `SetVimModeEnabled(bool enabled)`) that
     does two things every time it's called, regardless of caller: (a)
     sends the `WM_AR_TOGGLE_VIM` message to update the native
     `g_enableVimMode` flag immediately, and (b) updates
     `GeneralSettingsData.VimModeEnabled` and calls
     `SettingsService.SaveChanges()` so the new value is persisted, not
     just applied for the current session.
   - The settings-dialog checkbox's change handler AND the Command Palette
     "Toggle Vim Mode" `BaseCommand` must BOTH call this same function —
     there should be exactly one code path that changes Vim mode, with two
     entry points into it (slow/discoverable via Settings, fast/keyboard-only
     via the Palette), not two independent toggles that could fall out of
     sync with each other or with what's persisted.
   - On startup, load `VimModeEnabled` from settings the same way `AutoPair`
     is loaded, and apply it to each injected Application Designer process
     as part of the same setup flow that already applies AutoPairing in
     `AppDesignerProcess`/`EventHookInstaller`, so the very first editor
     opened already reflects the persisted choice rather than defaulting to
     off until something triggers a sync.
6. **Mode indicator via caret style, not a new permanent UI surface.** Use
   `SCI_SETCARETSTYLE` to switch between block caret (Normal/Visual) and line
   caret (Insert) — NppVim does this and it requires zero new UI. A status-bar
   text indicator ("-- NORMAL --") is a nice-to-have, not a requirement; skip
   it for v1 rather than inventing new UI plumbing. This does not remove the
   need for a visible `/`/`?` search prompt; search command text must be shown
   somehow while the user is typing it (see §6).

## 5. Features to port vs. strip vs. defer

### Port near-verbatim (no PeopleSoft/AppRefiner-specific concerns)
- All motions in `Motion.cpp` (h/j/k/l, w/W/b/B/e/E, 0/$/^, gg/G, {/}, %,
  page up/down, paragraph motions).
- All text objects in `TextObject.cpp` (`iw`, `aw`, `i"`, `a(`, `i{`, etc.).
- The operator-pending state machine in `NormalMode.cpp` (d/c/y combined with
  any motion or text object) — this is the single most valuable piece of the
  port; it's what makes Vim editing fast, not raw motions alone. **Count
  prefixes (`3j`, `5dd`, `2w`, etc.) are part of this state machine and are
  in scope for v1** — they thread through motions, operators, and registers,
  so they should be ported alongside the state machine rather than treated
  as optional.
- Visual mode (character/line/block) in `VisualMode.cpp`.
- Marks (`m`, `` ` ``, `'`) in `Marks.cpp` — single-file only (see below).
- Registers and macro recording/playback (`"ayy`, `"ap`, `qa...q`, `@a`).
- The `Keymap.cpp` trie matcher, to support `gg`/`dd`/etc. and (later) custom
  user-defined mappings.
- **Forward/backward search (`/`, `?`) and next/previous match (`n`, `N`)** —
  this is `CommandMode::performSearch`/`searchNext`/`searchPrevious` in
  `CommandMode.cpp` (lines 1077-1246), plus the search bookkeeping around
  `updateSearchHighlight`, `clearSearchHighlights`, `countSearchMatches`,
  and `showCurrentMatchPosition`. The search motion itself is pure Scintilla
  (`SCI_SEARCHINTARGET` and friends), zero Notepad++-specific calls. Port
  this as a core v1 feature — do NOT lump it in with the deferred `:`
  ex-command layer below, even though it lives in the same source file.
  Adapt the prompt/status/highlight pieces through AppRefiner (§6) rather
  than copying NppVim's status-bar and raw-indicator implementation verbatim.

### Strip or stub (genuinely incompatible with how Application Designer works)
- `gf` ("goto file under cursor") — `NormalMode.cpp` line ~268-317. Assumes a
  filesystem path under the cursor; meaningless for PeopleCode object
  references. Stub out or remove.
- Tab-switching commands (`NPPM_MENUCOMMAND` with `IDM_VIEW_TAB_NEXT`/`_PREV`
  in `NormalMode.cpp` around line 1303-1308) — Application Designer doesn't
  have Notepad++-style document tabs in the same sense. Remove; this need is
  better served by the separate buffer-cycling command described in §8.
- Block-comment toggle via `WM_COMMAND` to `nppData._nppHandle` (`NormalMode.cpp`
  ~line 1298, `VisualMode.cpp` ~line 1220) — targets Notepad++'s own main
  window command ID. If AppRefiner has an equivalent comment-toggle feature,
  retarget to call that instead; otherwise drop for v1.
- Cross-file mark jumping in `Marks.cpp` (the 3 `NPPM_GETFULLCURRENTPATH`/
  `NPPM_DOOPEN` call sites, lines ~57, ~181, ~184) — restrict marks to
  positions within the current editor/file only. Do not attempt to port
  "jump to a mark in a different file" — there is no direct AppRefiner
  equivalent of "open this file path" since PeopleCode objects aren't
  filesystem paths.
- `:e` (reload-from-disk / reopen file semantics) in `CommandMode.cpp` —
  **drop entirely.** This is file-path/document-buffer oriented Notepad++
  behavior and does not map cleanly to PeopleCode objects hosted inside
  Application Designer.

### Port as core v1 (confirmed zero Notepad++-specific calls, same category as `/` search)
- **`:s` substitution** — `CommandMode::performSubstitution` (lines ~843-960) is built
  entirely on `SCFIND_REGEXP` / `SCI_SEARCHINTARGET` / `SCI_REPLACETARGET`, verified by
  inspection. Port near-verbatim like the search engine already ported in step 6.
  - Support: `:s/pat/rep/flags`, `:%s/pat/rep/flags`, ranged forms `:5,12s/...`,
    `:.,$s/...`
  - Flags: `g` (all occurrences on line/range), `i`/`I` (case-insensitive),
    `l` (literal, not regex)
  - **Exclude the `c` (confirm each) flag** — NppVim implements it with a raw
    `GetMessage()` blocking loop (`Utils::getCharBlocking`). Running a nested message
    pump inside a subclass proc callback is risky in our context; leave `c` as a
    no-op (treat as `g`) or reject with an error message for now.

### Defer to a later iteration (not wrong to port, just not essential for v1)
- The rest of `CommandMode::handleColonCommand` — `:sort`, `:sp`/`:vs` splits,
  `:bn`/`:bp`/`:bd` buffer nav, `.rc` parsing, custom mappings. These are either
  Notepad++-specific or not meaningful in Application Designer.
- **Refined recommendation — the `:` layer is not all-or-nothing.** A pragmatic subset
  makes sense in App Designer even if the full NppVim command surface does not.
  Implement in Item 10 (see checklist):

  **In scope for Item 10:**
  - `:noh` / `:nohl` / `:nohlsearch` — clear search highlights
  - Bare numeric line jumps: `:42`
  - `:s` / `:%s` / ranged `:s` with flags `g`, `i`/`I`, `l` (see above)
  - `:set ignorecase` / `:set noignorecase` — flip case-sensitivity flag for
    `/` search and `:s`; just toggles a boolean
  - `:reg` / `:registers` — read-only display of register contents; show via
    `MessageBoxDialog`. **Depends on Item 7 (marks/registers) being complete.**
  - `:marks` — read-only list of set marks. **Depends on Item 7.**
  - `:delm a` / `:delm a-z` — delete one or a range of marks. **Depends on Item 7.**

  **Out of scope:**
  - `:w`, `:q`, `:wq`, `:x`, `:wqa`, `:qa` — Application Designer owns save/close;
    not safely triggerable from an injected hook.
  - `:e`, `:r filename`, `:sp`, `:vs`, `:split`, `:vsplit`, `:bn`, `:bp`, `:bd` —
    Notepad++ file/buffer/split model; no equivalent in Application Designer.
  - `:sort` / `:sort!` — routes through `nppData`/`IDM_EDIT_SORTLINES_*` (Notepad++
    menu commands); no free AppRefiner equivalent. Could be reimplemented as a
    pure-Scintilla line sort later, but not in scope now.
  - `:map`/`:nmap`/`:vmap`/`:imap`/`:noremap`, `:source`/`:so`, `:editrc`/`:erc`,
    `:editini`/`:eini`, `:config` — deferred `.rc`/custom-keymap config system.
  - `:help`, `:tutor` — no PeopleCode-relevant content; `K` tooltip covers this need.
  - `:about`, `:version`, `:donate`, `:paypal`, `:gh` — NppVim plugin-identity
    commands; meaningless once ported.
  - `:!command`, `:r !command` — shell execution from an injected process; excluded
    for safety.
  - `:wrap` / `:nowrap` — pure `SCI_SETWRAPMODE`; technically free to add but low
    priority and not in the standard cheat sheet.
- Keyboard layout auto-switching, configurable escape sequences (`jj`/`jk`),
  `.rc`/`config.ini`-style external configuration files. Pure polish; defer.

## 6. Required adaptations (small, mechanical, but must not be skipped)

- **Global-state refactor.** NppVim's code is organized around globals:
  `state`, `g_normalKeymap`, `g_visualKeymap`, `g_commandKeymap`,
  `g_insertKeymap`, and mode objects that often close over a `VimState&`.
  AppRefiner's port must not leave those globals pointing at one editor's
  state. Either introduce a `VimContext` passed into handlers/keymaps, or
  store keymaps/mode handlers inside each `VimEditorState`. Audit static
  locals and mutable globals copied from NppVim for the same reason.
- `Utils::getCurrentScintillaHandle()` in NppVim's `Utils.cpp` calls
  `NPPM_GETCURRENTSCINTILLA` on `nppData._nppHandle`. AppRefiner already
  knows the relevant `HWND` directly (it's the `hWnd` parameter passed into
  `ScintillaSubclassProc`) — this function becomes unnecessary; remove calls
  to it and use the parameter directly.
- `Utils::setStatus()` calls `NPPM_SETSTATUSBAR`. AppRefiner has no
  equivalent status bar. General mode/pending-key status can be omitted for
  v1, but `/` and `?` search prompts cannot be invisible. Provide a minimal
  visible prompt while the user types a search command, preferably by sending
  a one-way message to C# and reusing an existing AppRefiner tooltip/calltip
  or lightweight editor overlay path.
- **Search highlighting belongs to AppRefiner, not raw C++ indicators.**
  NppVim's highlight helpers directly use Scintilla indicator `0`.
  AppRefiner already allocates and tracks indicators from C# starting at
  indicator `0`, so a native Vim indicator would collide with existing search,
  bookmark, lint, and styler indicators. Route Vim search highlights through
  AppRefiner's existing `ScintillaEditor`/`ScintillaManager` highlighter
  machinery, or reserve a documented AppRefiner-owned Vim search highlighter
  there and have the C++ side request updates via a message. Do not copy
  NppVim's indicator `0` code verbatim.

## 7. Keybinding collisions — already checked, resolved below

The following were checked directly against AppRefiner's existing shortcut
registrations (both the native `ShortcutType` table in `Common.h` and every
`BaseCommand` in `AppRefiner/Commands/BuiltIn/`) as of this brief:

- **Already claimed, do not reuse:**
  - `Ctrl+H` — claimed natively by `SHORTCUT_SEARCH` (Find/Replace),
    `AppRefinerHook/HookManager.cpp` lines 881/884/1028/1031 (`wParam == 'H'`
    while `SHORTCUT_SEARCH` enabled).
  - `Ctrl+J` — claimed by `BetterFindCommand` as an alternate Find shortcut.
  - `Ctrl+K` — claimed by `BetterFindReplaceCommand` as an alternate
    Find/Replace shortcut.
  - `Ctrl+F`, `Ctrl+O`, `Shift+Up/Down` (line selection), `Ctrl+Shift+P`
    (Command Palette), `Ctrl+Alt+L` (Lint Current Code) — all claimed, listed
    here for completeness; don't reuse any of these for Vim-mode chords
    either.
- **Confirmed free, use these for the cross-instance feature (§8b):**
  `Ctrl+Shift+H` for "previous Application Designer instance", `Ctrl+Shift+L`
  for "next Application Designer instance". Neither appears in `Common.h`'s
  `ShortcutType` table (the only existing `Ctrl+Shift+` combo claimed
  natively is `Ctrl+Shift+P` for the Command Palette) nor in any built-in
  `BaseCommand` registration (the only existing `L`-binding anywhere is
  `Ctrl+Alt+L`, a different modifier set).

This resolves the open question from earlier discussion: keep
**`Shift+H` / `Shift+L` only** for previous/next PeopleCode editor *within*
the current Application Designer instance. These remain Normal-mode-only and
part of the Vim engine inside `ScintillaSubclassProc`'s Vim branch.

Still required before finalizing:
- Before wiring any new modifier-combo shortcut (including any NppVim
  Insert-mode Ctrl-chords such as Ctrl-W/Ctrl-U/Ctrl-T/Ctrl-D), test
  manually in a real Application Designer session to confirm it doesn't
  collide with one of Application Designer's own native menu accelerators —
  this isn't fully verifiable from source alone, since those accelerators
  live in Application Designer's own resources, not AppRefiner's.

## 8. One related but separable feature (do NOT build inside the Vim engine)

These came up in design discussion and are genuinely useful, but are
architecturally distinct from the Vim mode engine — implement them as
ordinary AppRefiner features, not as part of
`VimEditorState`/`ScintillaSubclassProc`'s Vim branch:

### 8a. Keyboard hover-info (`K` in Normal mode)
- Trivial addition once Vim Normal mode exists: bind `K` to call
  `TooltipManager.ShowTooltip(editor, position, lineNumber)` (C#,
  `TooltipProviders/TooltipManager.cs` line 441), feeding it the current
  caret position (`SCI_GETCURRENTPOS`) instead of a mouse-dwell position.
  This requires a small message from the C++ Vim handler back to C# (similar
  to how `WM_AR_FUNCTION_CALL_TIP` etc. already work) — this is the one
  legitimate case where the Vim engine needs to talk to C#, since the tooltip
  system itself (AST parsing, type inference) lives there. Include the
  Scintilla editor `HWND` and caret position/line in that message; the C#
  side should resolve the `ScintillaEditor` from `AppDesignerProcesses`
  rather than assuming `activeEditor` has already caught up to focus.

## 9. Build and test loop

AppRefiner already has a documented, working build pipeline — use it rather
than inventing a new one.

When adding the Vim C++ files, explicitly include the new `.cpp`/`.h` files
in `AppRefinerHook/AppRefinerHook.vcxproj`. If the port pulls in any NppVim
code that relies on C++17 library features (for example later config/option
code), set the hook project's C++ language standard explicitly rather than
depending on the Visual Studio default.

**Requirements** (per `README.md`): Windows, Visual Studio 2022 with C++
development tools, .NET 8 SDK, Java 17+ (ANTLR), PowerShell 5.1+.

**Full build** (use this after a batch of changes, or before any test
session that exercises both the C++ hook and the C# app together):
```powershell
.\build.ps1
```
This builds `AppRefinerHook.vcxproj` via MSBuild, builds the AppRefiner .NET
project, and stages everything (including `AppRefinerHook.dll`) into
`publish/framework/`. Run AppRefiner from that folder to test.

**Fast iteration loop for pure C++ changes** (use this while actively
porting/debugging the Vim engine — much faster than the full pipeline above):
1. Build only the hook project:
   ```powershell
   & "<path-to-MSBuild.exe>" "AppRefinerHook\AppRefinerHook.vcxproj" /p:Configuration=Debug /p:Platform=x64
   ```
   (Find `MSBuild.exe` via `vswhere.exe -latest -products * -requires Microsoft.Component.MSBuild -property installationPath`, then `MSBuild\Current\Bin\MSBuild.exe` under that path — `build.ps1` already does exactly this, lines ~67-79, copy the pattern.)
2. The rebuilt DLL lands at `AppRefinerHook\x64\Debug\AppRefinerHook.dll`
   (or `\Release\...` for a release build).
3. Copy it over the one already in your existing `publish/framework/`
   folder, replacing the old copy.
4. Close and relaunch Application Designer + AppRefiner to pick up the new
   hook DLL (it's loaded into the Application Designer process, so it can't
   be hot-swapped while that process is running).

**Note on who runs the build:** if Claude Code/Codex doesn't have a way to
actually run a Windows GUI app for manual testing in your environment, it's
fine — and expected — for it to stop after producing buildable code and let
you do the build + manual Application Designer test pass for each checklist
item below. Either way, the agent should always leave the repo in a state
that builds cleanly via the commands above before declaring a checklist item
done.

## 10. Implementation checklist

Work through this in order. Each item has a "build" and a "verify" — don't
move to the next item until verify passes. Check off items as you confirm
them; this is meant to be testable by you directly in Application Designer,
not just by the agent reading its own code.

- [x] **0. License attribution.** Add a short comment header crediting
      NppVim (https://github.com/h-jangra/NppVim, MIT) to each new ported
      file.
      **Verify:** the comment is present; no functional test needed.

- [x] **1. Minimal end-to-end path.** Add `g_vimEditorStates` map
      (`std::unordered_map<HWND, VimEditorState>`), a minimal
      `VimEditorState` struct (mode enum: NORMAL/INSERT/VISUAL, defaulting
      to NORMAL), and an initially small `VimSessionState` placeholder for
      the later shared registers/macros/search state. Add a persisted
      `VimModeEnabled` setting per §4 point 5 (settings checkbox + Command
      Palette toggle, both calling one `SetVimModeEnabled` function, which
      sends a new `WM_AR_TOGGLE_VIM` message from C# to flip a global
      boolean in `AppRefinerHook.dll`). This includes adding the setting to
      `Settings.settings`/`Settings.Designer.cs`. Inside
      `ScintillaSubclassProc`: when Vim mode is enabled and the editor's
      state is NORMAL, intercept `j` and map it to `SCI_LINEDOWN`,
      `return 0`; intercept `i` to switch mode to INSERT and call
      `SCI_SETCARETSTYLE` to a line caret; intercept `Esc` to switch back to
      NORMAL with a block caret. Everything else falls through unchanged.
      Add the `WM_NCDESTROY` cleanup (`g_vimEditorStates.erase(hWnd)`)
      alongside the existing `RemoveWindowSubclass` call (line 661).
      **Verify:** Build per §9. Open a PeopleCode editor in Application
      Designer. Confirm: (a) the caret is a block by default (Normal mode);
      (b) pressing `j` repeatedly moves the cursor down line by line and does
      NOT insert the letter "j"; (c) pressing `i` switches to a line caret
      and typing now inserts characters normally; (d) `Esc` returns to a
      block caret and `j` resumes moving the cursor; (e) autocomplete
      popups, call tips, and existing Ctrl/Shift/Alt shortcuts (e.g. the
      Command Palette) still work exactly as before in both modes; (f) two
      PeopleCode editors open at once have independent Normal/Insert states
      (switching one to Insert doesn't affect the other); (g) toggling Vim
      mode off via the settings checkbox makes `j` type the letter "j" again
      instead of moving the cursor; (h) toggling it back on via the Command
      Palette restores Vim behavior; (i) **close AppRefiner entirely and
      relaunch it** — confirm whichever on/off state you left it in (checked
      via either the checkbox or the palette command) is still in effect
      without needing to toggle it again, proving the setting actually
      persisted rather than just living in memory for that session.

- [x] **2. Core motions.** Port `Motion.cpp`. Wire up all single-key Normal
      mode motions: `h j k l`, `w W b B e E`, `0 $ ^`, `{ }`, `%`, page
      up/down, paragraph motions. Include count-prefix support now (e.g.
      `3j` moves down 3 lines) since it threads through everything that
      follows.
      **Verify:** in a real PeopleCode file, confirm each motion moves the
      cursor to the expected position, including with a numeric count
      prefix (e.g. `5j`, `3w`). Re-confirm Insert mode and existing
      AppRefiner features from step 1 still work.

- [x] **3. Multi-key sequences.** Port `Keymap.cpp`/`Keymap.h`, adapted so
      keymaps operate on the current `VimEditorState`/`VimSessionState`
      context instead of one global `VimState&`. Wire up `gg` (go to first
      line) and `G` (go to last line, or line N with a count prefix, e.g.
      `42G`).
      **Verify:** `gg` jumps to line 1, `G` jumps to the last line, `15G`
      jumps to line 15.

- [x] **4. Operators + text objects.** Port the operator-pending state
      machine in `NormalMode.cpp` and `TextObject.cpp`. Wire up `d`, `c`,
      `y` combined with motions (`dw`, `d$`, `dj`, etc.) and text objects
      (`diw`, `daw`, `ci"`, `da(`, etc.), plus `dd`/`yy`/`cc` (line-wise),
      `p`/`P` (paste), `x`/`X` (delete char), `r` (replace single char), `.`
      (repeat last change) if present in the ported state machine.
      **Verify:** test each of `dw`, `dd`, `yy`, `p`, `ciw`, `da(`, `x`, `r`
      followed by a character, and `3dd` (count + operator) on real
      PeopleCode. Confirm undo (`u`) reverts each correctly.

- [x] **5. Visual mode.** Port `VisualMode.cpp`. Wire up `v` (character
      visual), `V` (line visual), operators applied to a visual selection
      (`d`, `y`, `c` while a selection is active).
      **Verify:** `v` + motion selects text matching the motion; `V`
      selects whole lines; `d`/`y` while a selection is active operates on
      exactly the selected range; `Esc` exits visual mode back to Normal.

- [x] **6. Search.** Port `performSearch`/`searchNext`/`searchPrevious` from
      `CommandMode.cpp` (lines 1077-1246). Adapt the search prompt/status
      and highlight behavior through AppRefiner's existing UI/highlighter
      machinery per §6 instead of copying NppVim's raw indicator `0`
      helpers. Wire up `/` (forward search), `?` (backward/regex search per
      NppVim's existing convention — confirm which by reading the source),
      `n`/`N` (next/previous match). Do NOT port `handleColonCommand` or
      anything reachable only via `:` in this step.
      **Verify:** `/searchterm` + Enter jumps to the next match and
      highlights it; `n` repeats forward, `N` reverses direction; search
      wraps around at the end of the document; while typing `/searchterm`,
      the current search prompt is visible to the user.

- [x] **7. Marks and registers.** Port `Marks.cpp` (single-file only — strip
      the cross-file jump call sites per §5) and register/macro support
      (`"ayy`, `"ap`, `qa...q`, `@a`). Keep local marks in
      `VimEditorState`, but put registers and macro state in
      `VimSessionState` so the default/named registers work across editor
      switches inside the same Application Designer process. The clipboard
      register should use the OS clipboard.
      **Verify:** `ma` sets a mark, `` `a `` jumps back to it; named
      registers store/retrieve independently of the default register; yank
      in one PeopleCode editor and paste in another editor in the same
      Application Designer instance works; the clipboard register works
      outside AppRefiner too; recording a macro with `qa...q` and replaying
      with `@a` repeats the recorded actions correctly.

- [x] **8. Hover-info (`K`).** Implement per §8a.
      **Verify:** placing the cursor on a method/variable name and pressing
      `K` in Normal mode shows the same tooltip content that mouse-hovering
      over it shows today, even after switching between multiple open
      PeopleCode editors.

- [x] **9. In-instance editor cycling.** Implement `Shift+H` /
      `Shift+L` for previous/next PeopleCode editor within the current
      Application Designer instance. This is a Normal-mode-only Vim feature.
      **Verify:** with multiple PeopleCode editors open in the same
      Application Designer instance, `Shift+H` moves to the previous editor
      and `Shift+L` moves to the next editor; this only happens in Normal
      mode and does not interfere with existing AppRefiner/global shortcuts.

- [x] **10. `:` ex-command layer.** Implement the subset defined in §5.
      **In scope:**
      - `:noh` / `:nohl` / `:nohlsearch` — clear search highlights
      - Bare line jumps: `:42`
      - `:s/pat/rep/` and `:%s/pat/rep/` with flags `g`, `i`/`I`, `l`; range
        forms `:<n>,<m>s/...`, `:.,<n>s/...`, `:.,$s/...`; **`c` flag excluded**
        (nested message-pump risk inside subclass proc — omit or treat as `g`)
      - `:set ignorecase` / `:set noignorecase`
      - `:reg` / `:registers` (requires Item 7 complete)
      - `:marks`, `:delm <letter>`, `:delm <a-z>` (requires Item 7 complete)

      **Explicitly out of scope:** `:w`, `:q`, `:wq`, `:x`, `:wqa`, `:qa`,
      `:e`, `:r filename`, `:sp`/`:vs`, `:bn`/`:bp`/`:bd`, `:sort`, `:map`
      family, `:source`, config-file commands, `:help`, `:!`, `:wrap`.

Throughout: test against real PeopleCode editing sessions in Application
Designer specifically for interaction with existing AppRefiner features —
autocomplete popups, call tips, auto-pairing, the existing Ctrl/Shift/Alt
shortcut table — since these are the integration risks that can't be fully
verified by reading code alone.

## 11. AppRefiner-specific enhancements (beyond NppVim baseline)

These were added during implementation and have no direct equivalent in NppVim.
Record them here so future implementors know they exist and why.

### Hybrid relative line numbers (`VimLineNumbers.h` / `VimLineNumbers.cpp`)
While in Vim mode, margin 0 is repurposed as a right-aligned text margin
(`SC_MARGIN_RTEXT`) that shows hybrid line numbers: the absolute 1-based line
number on the current line (bold, dark), and relative distances on all other
lines (lighter gray). Updated on every `SC_UPDATE_SELECTION | SC_UPDATE_V_SCROLL`
notification in `HookManager.cpp`. Initialized when Vim mode is first activated
for an editor, shut down (margin cleared, margin type restored) when Vim mode is
disabled. The margin type is re-asserted on every `Update()` call because
Application Designer resets it if the user enables its own line-number display.

### `Ctrl+D` / `Ctrl+U` center cursor after half-page scroll
NppVim's `Ctrl+D` and `Ctrl+U` scroll a half-page without recentering. AppRefiner's
port calls `CenterCaret()` after the scroll (same behavior as `zz` — sets first
visible line to `currentLine - visibleLines / 2`). This makes repeated `Ctrl+D` /
`Ctrl+U` feel more stable since the cursor stays near the middle of the screen
rather than drifting to the edge. Both Normal and Visual mode variants center.

### AppRefiner-backed `gd` / `Ctrl+O` / `Ctrl+I` navigation
Normal mode adds a small AppRefiner-specific navigation layer that does not come
from NppVim directly:

- `gd` delegates to AppRefiner's existing `Go To Definition` implementation
- `Ctrl+O` delegates to AppRefiner's navigation-history "back" command
- `Ctrl+I` delegates to AppRefiner's navigation-history "forward" command

These are routed from the native Vim layer back into C# via custom window
messages (`WM_AR_VIM_GOTO_DEFINITION`, `WM_AR_VIM_NAV_BACK`,
`WM_AR_VIM_NAV_FORWARD`) and intentionally reuse AppRefiner's existing
navigation history rather than building a separate Vim-only jumplist.

One Windows/App Designer quirk is worth documenting: `Ctrl+I` may arrive as
`VK_TAB` rather than `'I'`, so the native hook treats both forms as the same
forward-navigation command while Vim mode is active.

### Expanded database-backed `Go To Definition`
During the Vim work, AppRefiner's existing `Go To Definition` pipeline was
extended so it can resolve more cross-file PeopleCode symbols when a database
connection is active. The important practical result is that Vim's `gd` and the
existing `F12` command now share the same broader navigation surface:

- cross-file Application Class definitions
- cross-file Application Class methods/properties
- Record definitions
- SQL definitions

Unresolved symbols intentionally fail silently so repeated navigation attempts
do not interrupt editing flow with modal dialogs.
