# AppRefiner Cross-Environment Editor Diff — Implementation Brief

## 0. Status: shipped (as-built summary)

**v1 is complete** (checklist §7 steps 1–5 all done). Sections 1–6 below are
the original forward-looking brief and are kept for rationale/history; where
this summary differs from them, this summary is authoritative.

What shipped:
- **`Services/EditorOpenTargetResolver`** — resolves the active PeopleCode/SQL
  editor to an `OpenTarget` via `OpenTargetBuilder.CreateFromCaption`.
- **`Services/ComparisonConnectionService` + `ComparisonConnectionSession`** —
  one temporary, read-only comparison DB per diff session, by reusing
  `DBConnectDialog` in a comparison mode (extra ctor params + a
  `focusDatabaseNameOnOpen` option). Disposed on close.
- **`Services/EditorComparisonService`** — fetches the comparison source
  (`GetPeopleCodeProgram` / `GetSqlDefinition`), builds a DiffPlex side-by-side
  model + hunks, applies hunks (PeopleCode: raw range; SQL: per-hunk in display
  space) wrapped in undo grouping, and recomputes on edit/refresh. Trailing
  newlines are stripped from display text so EOF doesn't show a phantom diff.
- **`Dialogs/ScintillaEditorControl`** — hosts a native Scintilla control
  (from AppRefiner's shipped `scintilla_mods` DLL) inside WinForms, exposing
  annotation alignment, background-marker line shading, a changed-span
  indicator, manual syntax styling, a line-number margin, scroll/edit
  notifications, and focus handling.
- **`Dialogs/ComparisonDiffDialog`** — the two-pane diff/merge UI: real
  document panes, annotation alignment (no text mutation), per-hunk pull
  arrows, live local editing (PeopleCode + SQL) with debounced recompute,
  synced scroll, App Designer-style syntax coloring, and line numbers. (This
  is a NEW dialog; the pre-existing `DiffViewDialog` was left untouched.)
- **Command palette:** `Debug*ComparisonDatabase*` commands drive connect /
  disconnect / show-OpenTarget.

Key decisions that changed from the brief: a single per-session comparison
connection (not a pool, §4.3); a new `ComparisonDiffDialog` rather than
extending `DiffViewDialog` (§2/§3); SQL is editable with normalized display
(not read-only). Optional, not built: hunk-to-hunk navigation and a
ComparePlus-style overview strip (§7 step 4 polish).

## 1. Goal

Add a feature to AppRefiner that lets you diff the single editor object
currently open in your active editor against the same object as it exists
in a different, already-named PeopleSoft environment (e.g. CSDEV vs
CSPRD, or CSFIX vs CSTST) — without needing a second Application
Designer instance open.

For v1, "editor object" means:
- one PeopleCode program currently open in a PeopleCode editor
- one SQL definition currently open in a SQL editor

Scope is deliberately narrow: this diffs exactly the one PeopleCode item
or SQL definition open in the active editor, not the whole
Record/Component/Project it belongs to. The comparison environment is
read-only for this feature —
no code path introduced here may call any write/save method against it.
Changes only ever flow in one direction: from the comparison environment
into the local (currently-open, already-connected) editor.

Important behavioral detail: the "local" side of the diff is the current
text in the active Scintilla editor buffer, not a fresh re-fetch from the
local database. This feature should show you what your current in-editor
code differs from in another environment, including unsaved local edits.
The comparison side is fetched from the selected comparison environment's
database using the same `OpenTarget`.

This is a pure C# feature. No native hook (`AppRefinerHook.dll`) changes
are required — everything here is achievable in the managed AppRefiner
project using infrastructure that already exists for other features.

## 2. Why this is tractable (what already exists and should be reused)

Every individual mechanism this feature needs already exists in the
codebase for some other purpose. This is an integration/wiring project,
not new infrastructure:

- **Standalone DB connections, independent of any open Application
  Designer window.** `OraclePeopleSoftDataManager`/`SqlServerPeopleSoftDataManager`
  (`AppRefiner/Database/OraclePeopleSoftDataManager.cs`,
  `SqlServerPeopleSoftDataManager.cs`) take a bare connection string in
  their constructor and have no dependency on an `AppDesignerProcess`.
  `DataManager` is already a property on `AppDesignerProcess`
  (`AppRefiner/AppDesignerProcess.cs` line 69), not a process-wide
  singleton — proving multiple independent live connections can already
  coexist in one AppRefiner process.
- **A working connection dialog to clone.** `AppRefiner/Dialogs/DBConnectDialog.cs`
  already does the full flow: environment name entry (with autocomplete
  from previously-used names), LDAP/direct connection handling,
  credential entry, DPAPI-encrypted optional password saving, and
  instantiates the right `IDataManager` based on DB type, exposing it via
  a public `DataManager` property. The new comparison-connection dialog
  should be a near-clone of this, not a from-scratch build.
- **Fetching a specific editor object's source by structural
  identity.** `IDataManager.GetPeopleCodeProgram(OpenTarget openTarget)`
  (`AppRefiner/Database/IDataManager.cs` line 248) returns the raw source
  string for one specific PeopleCode item. `IDataManager.GetSqlDefinition(string objectName)`
  already exists as the parallel fetch path for SQL definitions. `OpenTarget`
  (`AppRefiner/Database/Models/OpenTarget.cs`) identifies that item by
  type + a path of `(PSCLASSID, name)` segments — e.g. for record field
  PeopleCode: `[(RECORD, recordName), (FIELD, fieldName), (METHOD, eventName)]`
  (see the real usage in `AppRefiner/GotoDefintion.cs` line 90-97). This
  path is precise down to the single event (FieldChange vs FieldFormula
  vs FieldEdit, etc.) — it never represents "the whole record," so no
  extra scoping logic is needed to achieve the "just the PeopleCode I'm
  looking at" requirement; the existing identifier already is that scope.
  Crucially, this same `OpenTarget` is valid against any environment's
  `IDataManager` — same PeopleSoft object path, different database. SQL
  definitions follow the same pattern using `OpenTargetType.SQL`.
  For the current editor, AppRefiner already has a practical way to
  resolve that identity from the editor caption via
  `OpenTargetBuilder.CreateFromCaption(...)`, which is already used in
  other features. This should be the default v1 approach unless a more
  direct tracked identity is found and proves cleaner.
- **Persisted, named connection settings to extend.** `DBConnectDialog`
  already persists a `Dictionary<string, DbConnectionSettings>` (keyed by
  database name) as JSON in `Settings.Default.DbConnectionSettings`
  (`AppRefiner/Dialogs/DBConnectDialog.cs` lines ~1261-1299), via the
  standard `Properties.Settings.Default`/`user.config` persistence used
  elsewhere in the app. The comparison-environment picker should read
  from and write to this exact same saved dictionary — so an environment
  you've connected to before (whether as your primary or as a prior
  comparison target) is already in the picker's autocomplete history.
  **Caveat (verified):** both `DbConnectionSettings` (the value type) and
  the `savedSettings` dictionary are `private` members of `DBConnectDialog`,
  so they are not directly reachable from a separate picker class. This
  pushes toward §6's recommendation — reuse `DBConnectDialog` in a
  comparison mode and/or factor the settings load/save (and encryption)
  into a shared helper — rather than cloning the dialog and duplicating
  this state.
- **DPAPI password encryption to reuse as-is.** `EncryptPassword`/
  `DecryptPassword` (`DBConnectDialog.cs` lines ~1415-1471) use
  `System.Security.Cryptography.ProtectedData.Protect`/`Unprotect` with
  `DataProtectionScope.CurrentUser` and the DB name as entropy. This is a
  legitimate, OS-managed encryption mechanism (not plaintext, not a
  custom/weak scheme) — reuse it verbatim for comparison-environment
  passwords. No special-casing needed; match exactly what the primary
  connection already does, including the optional "Save Password"
  checkbox.
- **A diff-rendering dialog that already exists and is in active use.**
  `AppRefiner/Dialogs/DiffViewDialog.cs` already renders diffs with
  added/removed highlighting using DiffPlex (already a project
  dependency). **Correction (verified against the code):** it is NOT
  unwired — `AppRefiner/Dialogs/SnapshotHistoryDialog.cs` (line ~287)
  already constructs it via `DiffViewDialog(oldContent, newContent,
  title, owner)`. It also exposes a second constructor
  `DiffViewDialog(diffContent, title, owner)`. Note: DiffPlex supports
  line-level diffing with character-level sub-highlighting within
  modified lines (`SideBySideDiffBuilder` / inline diff builder model) —
  this is the closest available match to "Beyond Compare-style" diffing
  (contiguous changed-line blocks/hunks, with the specific changed
  characters within a modified line highlighted, not just whole-line
  highlighting). However, the current `DiffViewDialog` is still just a
  fairly simple read-only `RichTextBox` viewer, not a true side-by-side
  hunk UI.
  **Because it is shared with the snapshot-history feature, do NOT mutate
  it in place into the per-hunk apply UI.** Either (a) add the
  side-by-side/hunk capability as an additive, opt-in mode (new
  constructor/flag) that leaves the existing read-only `oldContent vs
  newContent` path untouched for `SnapshotHistoryDialog`, or (b) build a
  separate dialog for this feature and leave `DiffViewDialog` alone.
  Whichever path, keep using DiffPlex (and the finer-grained inline diff
  model) rather than introducing a different diff library, and re-test
  the snapshot-history diff afterward to confirm no regression.
- **Applying a change as a precise text edit, with correct undo
  grouping.** `AppRefiner/Refactors/TextEdit.cs` models exactly what a
  diff hunk's "pull this into CSDEV" action needs: `TextEdit(startIndex,
  endIndex, newText, description)` — replace a character range with new
  text. `BaseRefactor.ApplyEdits()` (`AppRefiner/Refactors/BaseRefactor.cs`
  lines 193-201) shows the existing pattern for applying a batch of edits
  safely: **sort edits descending by `StartIndex` and apply in that
  order**, so earlier edits in the document don't have their positions
  invalidated by later ones shifting text around. Reuse this exact
  ordering rule when applying multiple hunks.
  **Important gap to close, not assume:** the underlying call,
  `ScintillaManager.ReplaceTextRange` (`ScintillaManager.cs` line 454),
  does NOT wrap itself in undo grouping. `ScintillaManager.BeginUndoAction(editor)`
  / `EndUndoAction(editor)` (lines 410-423) exist and must be called
  explicitly by the new feature, bracketing the entire batch of hunks
  applied in one "pull from comparison" action — otherwise a single
  `Ctrl+Z` will only undo the last-applied hunk, not the whole action,
  which would violate the explicit undo requirement in §5.

## 3. Source material / files to point Claude Code / Codex at

No external repo is needed for this feature (unlike the Vim project) —
everything is internal AppRefiner code. Have the agent read these files
directly rather than working from this brief's paraphrase of them:

- `AppRefiner/Dialogs/DBConnectDialog.cs` — clone for the comparison
  connection dialog; reuse `EncryptPassword`/`DecryptPassword` verbatim
  (or factor them into a shared helper both dialogs call, which is
  cleaner — see §6).
- `AppRefiner/Database/IDataManager.cs`, `IDbConnection.cs`,
  `OraclePeopleSoftDataManager.cs`, `SqlServerPeopleSoftDataManager.cs` —
  the connection/data-access layer.
- `AppRefiner/Database/Models/OpenTarget.cs`, `PSCLASSID.cs` — the object
  identity model.
- `AppRefiner/GotoDefintion.cs` — reference for how an `OpenTarget` gets
  constructed from context; also worth checking how AppRefiner already
  knows "what PeopleCode item is this editor showing" when it originally
  loaded the editor's content, since that's the cleanest source of the
  `OpenTarget` for "the whole currently-open item" (see §5, open item 1).
- `AppRefiner/Dialogs/DiffViewDialog.cs` — existing diff renderer (reference
  only; left untouched — the shipped UI is the new `ComparisonDiffDialog` +
  `ScintillaEditorControl`, see §0).
- `AppRefiner/Refactors/TextEdit.cs`, `BaseRefactor.cs` (specifically
  `ApplyEdits()`) — the edit-application pattern and ordering rule.
- `AppRefiner/ScintillaManager.cs` — `ReplaceTextRange`,
  `BeginUndoAction`, `EndUndoAction`.
- `AppRefiner/Commands/BaseCommand.cs` and
  `AppRefiner/Commands/BuiltIn/BetterFindCommand.cs` — the pattern for
  registering the new "Diff Current Editor Against..." Command
  Palette entry.
- `AppRefiner/Services/SettingsService.cs` — if any new non-connection
  settings are needed (unlikely; comparison connections persist via the
  existing `DbConnectionSettings` dictionary, not new settings fields).

## 4. Architecture decisions to lock in before writing code

1. **Trigger: Command Palette only for v1.** A new `BaseCommand`
   ("Diff Current Editor Against...") invoked via `Ctrl+Shift+P`. No
   margin-icon or toolbar-button trigger for v1 — AppRefiner has never
   injected UI into Application Designer's own menu/toolbar chrome
   (checked: no such code exists today), and a margin-click trigger,
   while technically feasible via the already-used `SCN_MARGINCLICK`
   mechanism, is a mouse-first interaction that doesn't fit this
   project's otherwise keyboard-first direction. Treat a margin icon as
   a possible v2 addition, not a v1 requirement.
2. **No standing/persisted "comparison target" — prompt every time.**
   Every invocation of the diff command must prompt for which environment
   to compare against (the user's stated workflow varies: CSDEV→CSPRD one
   time, CSFIX→CSTST another). Do not build a "set it once" persistent
   default.
3. **One temporary comparison connection per diff session.** Do not add
   a long-lived pool of simultaneously-active comparison connections for
   v1. Each invocation of the diff feature may prompt for a comparison
   environment, open a single comparison `IDataManager`, use it for the
   life of that diff session, and then dispose/disconnect it when the
   dialog closes (or when the user explicitly disconnects from within
   that flow, if such a control exists). This keeps the workflow aligned
   with the actual use case: compare against one environment, finish,
   disconnect, and later choose a different environment if needed.
4. **Strictly one-directional, read-only-on-the-far-side.** The
   comparison `IDataManager` for any environment must only ever be used
   to call read operations (`GetPeopleCodeProgram`, `GetSqlDefinition`,
   and whatever check is
   used for "does this object exist" — see §5). No code path in this
   feature may call a write/save method on a comparison connection. The
   only thing ever written is the local, already-open editor's Scintilla
   buffer (via the existing edit-application pattern in §2/§6) — never
   anything sent back to the comparison environment's database.
5. **Undo must work, and must undo a whole "pull" as one action.** Wrap
   every batch of hunk-applications in `ScintillaManager.BeginUndoAction`/
   `EndUndoAction` (see the gap noted in §2). A single `Ctrl+Z` after
   pulling one or more hunks from the comparison side must revert all of
   them in one step, not one hunk at a time. This needs an explicit test,
   not just an assumption that Scintilla's default grouping handles it.
6. **Diff rendering: line-level hunks with character-level highlighting
   within modified lines, side-by-side.** This is the closest match to
   "how Beyond Compare presents a diff" without reverse-engineering Beyond
   Compare specifically. Use DiffPlex's existing capability for this (see
   §2) rather than introducing a new diff library, since DiffPlex is
   already a project dependency, used today by `DiffViewDialog` /
   `SnapshotHistoryDialog`. Per §2, add the hunk UI additively or in a new
   dialog — do not break the shared `DiffViewDialog` that snapshot history
   depends on. The comparison dialog should use real selectable text panes,
   not a grid-per-line presentation, so manual selection/copy remains
   possible for complicated diffs.
7. **Per-hunk granularity for the "pull into local" action**, not
   whole-file replace only. Each contiguous changed block gets its own
   action control (button/arrow) to pull just that hunk's comparison-side
   text into the local editor at the corresponding position. This is the
   single largest piece of new UI/logic work in this feature — budget for
   it accordingly (see §7 checklist staging, which deliberately sequences
   "view-only diff" before "per-hunk pull" so the harder interactive piece
   is validated against a working, simpler baseline first).
8. **Refresh the diff after any apply action.** Once one or more hunks are
   pulled into the local editor, the old diff model is stale: offsets may
   have shifted, neighboring hunks may merge/split, and already-applied
   hunks may disappear entirely. The dialog must recompute the diff
   immediately after each apply action (preferred), or else close and
   require the user to reopen it. Recomputing in place is preferred
   because it keeps the hunk map honest without inventing custom offset-
   remapping logic. When re-rendering, preserve the user's approximate
   scroll position so applying one hunk does not snap the view back to the
   top of the document.
9. **Diff interaction should feel editor-like, not form-like.** The final
   v1 UI should have two synchronized document panes, a narrow center
   gutter with directional apply arrows, explicit Undo and Refresh
   controls, and keyboard copy behavior that works naturally inside either
   pane. Avoid row-by-row button grids; they make selection, scanning, and
   manual comparison meaningfully worse.
10. **The panes must be real document views, not aligned text snapshots.**
   The editable local side should be backed by the real local document
   text, and the comparison side by the fetched comparison text. Visual
   alignment, spacer rows, hunk shading, and changed-span emphasis should
   be produced by a separate diff/view layer rather than by mutating the
   underlying pane text. This is the key architectural requirement if the
   tool is going to behave more like Beyond Compare than like a rendered
   report.
11. **Local editing should update the diff smoothly without round-tripping
   the comparison side.** Editing the local pane should update the real
   local App Designer editor and recompute the diff against a cached
   comparison snapshot after a short debounce. Do not re-fetch the remote
   document on every local keystroke. Avoid full dialog teardown or
   scroll-reset effects; re-render in place as much as practical.
12. **The current RichTextBox-based aligned-view prototype is not the final
   path.** If the implementation still redraws or rebinds the entire pane
   on ordinary local typing, that prototype should be treated as a dead
   end and replaced rather than iteratively patched. Step 4 should then be
   understood as an editor-surface architecture pass, not just a visual
   polish pass.
13. **Preferred replacement architecture: embedded local Scintilla panes.**
   The next serious implementation pass should investigate hosting two
   local Scintilla-based editor controls directly inside the diff dialog:
   one editable local pane and one read-only comparison pane. If that is
   feasible without a major detour, it should become the foundation of the
   final diff UI.
14. **Exact-position apply in v1, not fuzzy patching.** A hunk apply action
   should operate only against the exact local snapshot used to build the
   current diff model. Before applying, verify the editor buffer still
   matches that local snapshot (or otherwise ensure the diff was freshly
   recomputed from the current buffer). If the local text has changed in a
   way that makes the current diff stale, force a refresh instead of
   attempting approximate/fuzzy hunk relocation. Defer patch-style
   heuristics, partial rejects, and 3-way merge behavior to a later
   version.
15. **SQL diffs need normalization-aware display.** AppRefiner already
   auto-formats SQL in the enhanced editor path (`ApplyBetterSQL` on
   editor init and after save when `BetterSQL` is enabled), while
   comparison SQL fetched from the database is raw stored text. That means
   two semantically-identical SQL definitions can look completely
   different in a raw text diff. For SQL only, the diff display path
   should always compare normalized text produced by the same
   AppRefiner SQL formatting rules/config used in the editor, while still
   retaining raw local/remote SQL text separately for any future apply
   operation. Do not generalize this rule to PeopleCode unless a real
   AppRefiner PeopleCode formatter is introduced later; current evidence is
   that AppRefiner has SQL formatting only, not equivalent PeopleCode
   whole-buffer formatting.

## 5. Open implementation details the agent must resolve by reading code
   (not invent from scratch)

1. **How to get the `OpenTarget` for "the entire item currently open in
   this editor"**, as opposed to `GotoDefinition`'s use case (resolving
   the target of some symbol referenced inside the code). AppRefiner had
   to know the `OpenTarget`-equivalent identity of a PeopleCode item
   already, in order to have fetched and loaded it into the editor in the
   first place when the user opened it in Application Designer. Find
   where that original identity is captured (likely on `ScintillaEditor`
   or `AppDesignerProcess`, possibly derived from window title parsing,
   an existing "what is currently open" tracking mechanism, or similar)
   and reuse it directly, rather than re-deriving it via AST analysis the
   way `GotoDefinition` does. Current evidence suggests
   `OpenTargetBuilder.CreateFromCaption(editor.Caption)` already covers the
   main PeopleCode editor types and is likely sufficient for v1. If a more
   direct tracked identity exists, prefer it; otherwise use the existing
   caption parser rather than inventing a new inference path.
2. **How to detect "this object doesn't exist in the comparison
   environment"** cleanly, as distinct from a connection error or other
   failure. Check what `GetPeopleCodeProgram` actually returns/throws
   when the queried object has no row in that environment's database
   (null return vs. exception vs. empty string) and design the "not
   found in {environment}" UI state around whichever it actually is —
   don't assume. **Verified:** `GetPeopleCodeProgram` returns `string?`,
   and the Oracle/SQL Server implementations catch/log internal failures
   and then return `null` — so "not found" and "fetch failed" are NOT
   cleanly distinguished today. This needs a small supporting change
   (distinct signaling for the two cases) before the "not found in
   {environment}" UX can be definitive; do it early, in step 3.

## 6. Suggested factoring (not strictly required, but recommended)

- Factor `EncryptPassword`/`DecryptPassword` out of `DBConnectDialog` into
  a small shared static helper (e.g. `AppRefiner/Database/CredentialProtection.cs`)
  that both the existing dialog and the new comparison-connection dialog
  call, rather than duplicating the DPAPI logic into a second copy. Low
  risk, removes duplication, and means any future fix to this logic only
  needs to happen once.
- Consider whether the comparison-connection picker can literally be the
  existing `DBConnectDialog` reused in a different mode (e.g. a
  constructor flag like `isComparisonConnection: true` that changes only
  the dialog title/copy and what it does with the resulting `IDataManager`
  on close), rather than a separate near-duplicate class. This is worth
  evaluating once the agent has read `DBConnectDialog.cs` in full — if
  the dialog's internals are reasonably decoupled from "this becomes the
  primary connection," reuse is cleaner than cloning; if they're tightly
  coupled, cloning is more pragmatic than fighting that coupling.
- Keep the first usable version of the diff viewer intentionally simpler
  than the final ambition. A correct view-only diff should land before the
  more ambitious per-hunk apply UI. The side-by-side hunk interaction is
  the most expensive part of the feature; validate the data flow first.

## 7. Implementation checklist

Each item includes what to build and how to verify it. Don't move to the
next item until verify passes.

- [x] **1. Resolve active editor identity.** Use
      `OpenTargetBuilder.CreateFromCaption(editor.Caption)` as the initial
      implementation path for getting the `OpenTarget` of the entire
      currently-open PeopleCode item or SQL definition. Add a small diagnostic (e.g. a
      temporary debug command, or a log line) that prints the resolved
      `OpenTarget` for whatever editor is active. If this fails for a real
      editor type in testing, treat that as a parsing-support gap to fix
      before moving on.
      **Verify:** open several different kinds of editors (a Record
      Field event, an Application Class method, a Component event, and a
      SQL definition) and confirm the resolved `OpenTarget` correctly
      identifies each one specifically — not the parent
      Record/Component/Package, and not the wrong SQL object/type.

- [x] **2. Comparison connection dialog + session lifecycle.** Build
      (or adapt, per §6) the comparison-connection dialog: prompts for
      environment name (with autocomplete from the existing
      `DbConnectionSettings` saved dictionary), credentials, optional
      "Save Password" (DPAPI, matching the primary connection exactly).
      Open one comparison `IDataManager` per diff session and dispose/
      disconnect it when the session ends, without disturbing the primary
      connection. Until the full diff window exists, it is reasonable to
      expose this lifecycle through Command Palette entries such as
      `Diff: Connect Comparison Database` and
      `Diff: Disconnect Comparison Database`.
      **Verify:** connect to a comparison environment, confirm credentials
      save/restore correctly (with "Save Password" on and off); complete a
      diff session and confirm the comparison connection can be closed
      cleanly; start a later diff against a different environment and
      confirm it works independently of the earlier session; confirm
      AppRefiner did not need to restart at any point in this sequence.

- [x] **3. Fetch and diff (view-only, no apply yet).** Extend the Command
      Palette `Diff: Connect Comparison Database` flow so that after
      connecting (or when reusing an already-connected comparison
      session), it resolves the local `OpenTarget` (step 1), reads the
      local side from the current editor buffer, fetches the comparison
      side from the chosen comparison `IDataManager` (using
      `GetPeopleCodeProgram` for PeopleCode and `GetSqlDefinition` for
      SQL definitions), and renders the result. Per §2, do this
      WITHOUT breaking the `DiffViewDialog` that `SnapshotHistoryDialog`
      already uses — add an additive opt-in mode or a new dialog. Start
      with correctness first; if needed, land the initial pass as a better
      read-only diff before the full side-by-side hunk UI. Also land the
      §5.2 not-found vs fetch-failed signaling change here, since this
      step's "not found" verify depends on it.
      **Verify:** diff a PeopleCode item that has real differences between
      two environments and confirm added/removed/modified lines are
      visually distinct and accurately reflect the actual content
      differences; diff an item that is identical in both environments
      and confirm a clean "no differences" state; diff an item that
      exists locally but not in the comparison environment and confirm
      the §5.2 "not found" handling triggers correctly rather than an
      error or a blank/incorrect diff.
      **Follow-up note for later UI work:** SQL needs an always-on normalization-aware
      compare mode because AppRefiner may already have reformatted the
      local editor text while the comparison side remains raw database
      text. Revisit this in the next diff-UI pass so semantically-equal
      SQL does not show as a full diff purely due to formatting.

- [x] **4. Replace the current diff surface with an editor-style document
      architecture.** The current prototype proved that diff/apply wiring
      works, but it redraws far too aggressively and treats the pane text
      itself as the aligned diff. Replace that with a design where the
      local and comparison sides are real document panes, while alignment,
      hunk shading, changed-span emphasis, and gutter arrows are layered on
      top as view state. This is the gating work before step 4 can be
      considered complete.

      **Spike result (resolved):** the embedded-Scintilla feasibility spike
      SUCCEEDED — a native Scintilla control hosts inside the AppRefiner
      WinForms diff dialog, and blank-line annotations (alignment),
      background markers (line shading), and an indicator (changed spans)
      all render correctly with no document-text mutation. The
      Scintilla-panes architecture (§4.13) is therefore ADOPTED, not
      hypothetical. Implementation lives in
      `AppRefiner/Dialogs/ScintillaEditorControl.cs` (native host) and the
      rewritten `AppRefiner/Dialogs/ComparisonDiffDialog.cs` (two panes,
      annotation alignment, markers/indicators, synced scroll, per-hunk
      pull). The RichTextBox prototype has been replaced in place.

      **Subtasks for step 4:**
      - [x] run a feasibility spike for two embedded local Scintilla panes
        in the diff dialog
      - [x] if feasible, adopt Scintilla as the pane/control strategy
      - [x] separate raw document text from visual diff alignment/mapping
        (panes hold real text; alignment is annotations)
      - [x] keep the comparison side cached and read-only
      - [x] render hunk shading, changed spans, and gutter arrows as overlays
        / indicators / markers rather than by mutating pane text
      - [x] keep synchronized scrolling without tearing the view down on
        every change
      - [x] preserve per-hunk apply and grouped undo on top of that model
      - [x] **make local typing update the local document and recompute the
        diff after a short debounce without rebinding the entire surface**
        — the local (left) pane is editable for both PeopleCode and SQL;
        edits debounce (250ms), push to the real App Designer editor via
        `updateLocalText`, and re-decorate in place (no pane reload,
        caret/viewport preserved), with a full-rebind fallback only if
        round-tripped text drifts (EOL). For SQL, panes are normalized at
        load/refresh; edits show as-typed (not re-normalized mid-edit, which
        would reflow under the caret) and re-normalize on the next Refresh —
        safe because App Designer canonicalizes SQL formatting on save.
      - [x] alignment verified: top-of-file changes align correctly (the
        Scintilla line-0 annotation limitation isn't a problem in practice),
        and trailing-blank-line mismatches between the editor buffer and the
        DB copy are normalized away (display text is stripped of trailing
        newlines; the apply staleness check is trailing-insensitive) so
        end-of-file no longer shows a phantom diff. Intra-line indicator
        spans use UTF-8 byte offsets (correct for ASCII; non-ASCII unverified
        but a non-issue for PeopleCode/SQL).
      - [x] line numbers in both panes (Scintilla number margin; alignment
        gap lines are correctly unnumbered).
      - [x] for SQL, normalized display comparison with raw text kept for
        apply semantics; SQL apply is per-hunk in display space, with edits
        and per-hunk pull verified end-to-end against real SQL objects.
      - [x] syntax coloring in the panes (App Designer scheme: keywords +
        built-in functions blue, comments green, strings red). Manual
        styling — PeopleCode via the self-hosted `PeopleCodeLexer` (+
        `PeopleCodeTypeDatabase.GetFunction` for built-ins), SQL via a small
        scanner; no Lexilla dependency. Re-applied on edit/refresh.
      - [ ] polish (optional, post-baseline): hunk-to-hunk navigation
        (n/N or F8); a ComparePlus-style overview/nav strip (v2, see §8.1)
      **Verify:** typing a single character in the local pane should not
      cause a visible full-pane redraw; newly introduced differences should
      appear smoothly after the debounce; manual selection/copy should work
      naturally; applying a hunk should update only the affected region and
      keep the viewport stable; `Ctrl+Z` should still undo the last apply as
      one action; SQL formatting-only differences should not create noisy
      applyable hunks.

- [x] **5. Confirm read-only enforcement.** Code-review pass complete: the
      comparison `IDataManager` is only ever called for reads
      (`GetSqlDefinition`, `GetPeopleCodeProgram` in `FetchComparisonText`)
      and `Disconnect()` on session dispose. Every write
      (`ReplaceTextRange` / undo grouping) targets the local App Designer
      editor only — never the comparison connection. Read-only by
      construction.
      **Optional manual confirmation:** re-fetch + re-diff a comparison
      object immediately after a full diff + pull-hunk session and confirm
      the comparison side is byte-identical to before.

Throughout: since this feature touches real PeopleSoft database
connections and applies edits to real PeopleCode, test against
non-critical/sandbox environments first, and confirm the "pull into
local" action never fires accidentally (e.g. double-click protection,
clear visual distinction between "view" and "apply" controls) given it
mutates code you may not have intended to change yet.

## 8. Research findings & prior art (in progress)

This section accumulates external research that should inform the step-4
rendering/interaction rearchitecture. It is reference material, not
committed design — promote anything here into §4 decisions once it is
settled.

### 8.0 Target interaction model: merge-tool ergonomics, directional
The UX we are actually aiming for is a **3-way-merge-tool-style**
interface (think Beyond Compare's merge view, KDiff3, Meld 3-way,
Araxis Merge), NOT a read-only diff report. The important caveat:

- The comparison/remote pane (fetched from the other environment's DB) is
  **read-only**, and there is no common-ancestor "base" version available.
- So this is not a true 3-way merge. Functionally it is a **directional
  2-way merge**: changes only ever flow remote → local, and the only
  editable surface is the local pane (which is the live App Designer
  editor buffer).
- We still want the merge-tool *ergonomics*: two aligned document panes, a
  center gutter with directional apply controls per change block, live
  editing on the local side, and synchronized navigation/scroll.

Further research to do here: look at how the established merge tools above
present a 2-of-3 (one side locked) merge, and how they handle "accept
this block from the other side" affordances and re-diff-on-edit. Capture
findings as 8.x subsections.

### 8.1 Prior art: ComparePlus (Notepad++ Compare plugin) — GPL, clean-room only
ComparePlus is the closest direct prior art: it diffs two **Scintilla**
documents side-by-side, exactly the surface step 4 targets. Source was
reviewed locally (the `comparePlus-cp_3.0.0/` reference tree, which is
gitignored, not vendored).

**LICENSE CONSTRAINT (hard):** ComparePlus is **GPL v3**; AppRefiner is
MIT. We may **not** copy or adapt any ComparePlus code — doing so would
force AppRefiner to become GPL. Everything below is pattern/architecture
only, to be reimplemented clean-room in C# from understanding. Do not
transcribe its source.

**How it renders a diff — all without mutating document text** (this is
the property §4.10 / §4.12 require, and ComparePlus shows the concrete
Scintilla mechanism for each):

- **Alignment = blank line annotations.** Documents stay pristine; blank
  styled annotations (`SCI_ANNOTATIONSETTEXT`, with a dedicated extended
  style allocated via `SCI_ALLOCATEEXTENDEDSTYLES` /
  `SCI_ANNOTATIONSETSTYLEOFFSET` / `SCI_STYLESETEOLFILLED`) are added
  below lines to push the opposite side's matching lines into horizontal
  alignment. An "alignment info" list of `(mainLine, subLine, diffMask)`
  pairs drives it; annotation counts are set until
  `VISIBLEFROMDOCLINE(main) == VISIBLEFROMDOCLINE(sub)` for paired lines.
  **Gotcha:** Scintilla cannot annotate line 0, so line-0 alignment is
  special-cased.
- **Line shading = background markers.** `SCI_MARKERDEFINE … SC_MARK_BACKGROUND`
  + `SCI_MARKERSETBACK` for added/removed/changed/moved lines; applied
  with `SCI_MARKERADD`, cleared with `SCI_MARKERDELETEALL`. Per-line color
  with no text styling.
- **Changed-span emphasis = an indicator.** A Scintilla indicator
  (`INDIC_ROUNDBOX`, alpha, `SC_INDICFLAG_VALUEFORE`) filled over the exact
  changed character ranges within a modified line.
- **Gutter icons = RGBA image markers.** `SCI_MARKERDEFINERGBAIMAGE`
  routed to a margin via `SCI_SETMARGINMASKN` (added/changed/removed/moved
  icons, plus an arrow symbol) — the natural place to mark an actionable
  hunk.
- **Scroll sync / navigation** rides on a doc↔visible line mapping helper
  set (`SCI_VISIBLEFROMDOCLINE` / `SCI_DOCLINEFROMVISIBLE`, accounting for
  annotations and wrap). A separate overview/navigation bar renders a
  whole-file "map" of diff locations (a possible v2 nicety for us).

**Two caveats before leaning on it:**

1. ComparePlus uses Notepad++'s **built-in** dual editor views
   (`MAIN_VIEW` / `SUB_VIEW`), not Scintilla controls hosted inside a
   dialog. It therefore proves the annotation/marker/indicator rendering
   approach is correct, but does **not** validate §4.13's premise of
   embedding two Scintilla controls in an AppRefiner WinForms dialog — nor
   where that dialog's Scintilla even comes from (AppRefiner's own process
   has no Scintilla; App Designer's lives cross-process). That feasibility
   spike (§7 step 4, first subtask) remains the true gating decision, and
   this whole clean rendering approach is only available if it succeeds.
2. ComparePlus is a **viewer/navigator, not a merge tool** — it has no
   "apply this block to the other side." So there is no prior art to
   borrow for our directional apply; only the visual vocabulary (mark the
   actionable hunk with a margin arrow) transfers.

**Net takeaway:** if the embedded-Scintilla spike succeeds, adopt the
ComparePlus rendering vocabulary clean-room — annotations for alignment,
background markers for line shading, an indicator for changed spans, RGBA
margin markers for gutter affordances, and visible/doc line mapping for
sync. This replaces the prototype's text-mutation alignment (the §4.12
dead end) with a real view layer.

### 8.2 Prior art: VS Code (microsoft/vscode) — MIT, reference-friendly
VS Code's built-in compare is the experience we are ultimately trying to
mimic. Source reviewed via GitHub (not vendored locally).

**LICENSE (favorable):** the `microsoft/vscode` repository is **MIT** —
only the Microsoft-branded *build* is proprietary. So unlike ComparePlus
(GPL), we may reference and adapt its logic within MIT compatibility. It
is TypeScript on the Monaco editor, so we reimplement concepts in C#
regardless, but we are not restricted to clean-room-from-memory.

**It is actually two distinct features — and we want a blend of both:**

1. **Diff editor** (side-by-side compare) — the closest match to our
   render. Key pieces:
   - Diff data model in `src/vs/editor/common/diff/rangeMapping.ts`: a
     list of `LineRangeMapping`, each carrying *nested* character-level
     `RangeMapping`s. This is a cleaner two-tier (line + char) model than
     DiffPlex's line-piece output, where we currently derive char spans
     ourselves (`FindChangedSpan`). DiffPlex remains adequate; note the
     richer model only if our char-span heuristic proves insufficient.
   - **Alignment = view zones / virtual filler lines**, not text mutation
     — the same mechanism as ComparePlus annotations, independently
     confirmed. Monaco view zones ≈ Scintilla annotations.
   - **Two-tier highlighting**: light line-background for added/removed/
     changed, plus a darker character-level highlight for the exact
     changed span (decorations in
     `src/vs/editor/browser/widget/diffEditor/registrations.contribution.ts`
     + `style.css`). Maps to Scintilla background-markers (line) +
     indicator (char) — the §8.1 split.
   - Side-by-side and inline (unified) layouts with synchronized scroll.

2. **Merge editor** (true 3-way) — informs the *interaction state* model,
   but most of it does NOT apply to us. It manages four docs (Base,
   Input1=current, Input2=incoming, Result) and a `ModifiedBaseRange` per
   conflict with observable state
   `accepted ∈ {base, input1, input2, both, unrecognized}` + per-input
   `handled` flags; accepting a resolution runs `getEditForBase(state)` →
   removes conflicting diffs from Result → `applyEditRelativeToOriginal()`.
   **Why most of it doesn't apply:** our feature has **no base/common
   ancestor** and only **one editable side**, so there is no two-input
   conflict to resolve. Our model collapses to "for each diff between
   local(=Result) and remote, accept-incoming = replace the local range
   with remote text" — exactly what `EditorComparisonService.ApplyHunk`
   already does. What genuinely transfers:
   - The **observable, recompute-on-result-change** pattern (the model
     re-derives conflict/diff state when Result changes, rather than
     tearing down and rebuilding) — corroborates §4.8 and §4.11
     (recompute in place after apply/edit; debounce local edits).
   - The `getEdit → apply-to-result` edit shape — same as our TextEdit /
     `ReplaceTextRange` apply.
   - The `unrecognized` state idea (manual edits no longer matching a
     clean resolution). Our simpler equivalent is §4.14: detect a stale
     local snapshot and force a refresh instead of fuzzy relocation.

**Reframed target (supersedes the loose "3-way merge" wording in §8.0):**
what we want is VS Code's **diff-editor rendering** (view-zone alignment +
two-tier highlighting + synced scroll) plus a **single-direction
"accept incoming" apply** distilled from the merge editor — without the
base/input1/input2 conflict-resolution machinery, which a directional,
no-ancestor, read-only-remote feature does not need.

**Clean-room reference port worth studying:** `esmuellert/codediff.nvim`
reimplements VS Code's diff algorithm (Myers + char-level refinement) and
its two-tier rendering in a **non-Monaco** editor (Neovim) — i.e. the same
"port VS Code diff logic to a different editor surface" problem we have. It
explicitly mirrors `rangeMapping.ts` data structures and the
`registrations.contribution.ts` decoration model, and uses virtual filler
lines for side-by-side alignment. Useful as a concrete, digestible model
for how the VS Code concepts survive translation to a non-Monaco control.

**Files for a future deeper dive:**
- `src/vs/editor/common/diff/rangeMapping.ts` — line+char diff data model
- `src/vs/editor/browser/widget/diffEditor/` — diff rendering, view-zone
  alignment, decoration registrations
- `src/vs/workbench/contrib/mergeEditor/browser/model/mergeEditorModel.ts`
  — 3-way state model (for the observable/recompute pattern only)
