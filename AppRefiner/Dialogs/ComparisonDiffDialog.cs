using AppRefiner.Services;
using DiffPlex.DiffBuilder.Model;
using PeopleCodeParser.SelfHosted.Lexing;
using PeopleCodeTypeInfo.Database;
using System.Text;

namespace AppRefiner.Dialogs
{
    /// <summary>
    /// Side-by-side comparison dialog backed by two hosted Scintilla panes. Alignment is done with
    /// blank line annotations (not by mutating document text), line changes with background markers,
    /// and intra-line changes with an indicator — the clean-room rendering vocabulary distilled from
    /// ComparePlus and VS Code (see DIFF_TOOL.md §8). Per-hunk gutter arrows pull a hunk's
    /// comparison-side text into the local editor.
    ///
    /// The local (left) pane is editable; edits are debounced, pushed to the real App Designer
    /// editor, and the diff is recomputed and re-decorated in place without reloading the pane (so
    /// the caret/viewport stay put). The comparison (right) pane is always read-only. SQL panes show
    /// normalized text at load/refresh; edits show as-typed (re-normalized only on the next Refresh,
    /// since App Designer canonicalizes SQL formatting on save), and SQL applies use the whole-
    /// definition path.
    /// </summary>
    public sealed class ComparisonDiffDialog : Form
    {
        private const int MARKER_ADDED = 20;
        private const int MARKER_REMOVED = 21;
        private const int MARKER_CHANGED = 22;
        private const int INDICATOR_CHANGE = 8;

        // Syntax style numbers (0 = default/black). Matches Application Designer's scheme:
        // keywords AND built-in functions blue, comments green, string literals red.
        private const int STYLE_KEYWORD = 1;
        private const int STYLE_COMMENT = 2;
        private const int STYLE_STRING = 3;

        private static readonly Color KeywordColor = Color.FromArgb(0, 0, 255);
        private static readonly Color CommentColor = Color.FromArgb(0, 128, 0);
        private static readonly Color StringColor = Color.FromArgb(200, 0, 0);

        private static readonly HashSet<string> SqlKeywords = new(StringComparer.OrdinalIgnoreCase)
        {
            "SELECT", "FROM", "WHERE", "AND", "OR", "NOT", "NULL", "IS", "IN", "LIKE", "BETWEEN",
            "ORDER", "BY", "GROUP", "HAVING", "DISTINCT", "AS", "JOIN", "LEFT", "RIGHT", "INNER",
            "OUTER", "FULL", "CROSS", "ON", "UNION", "ALL", "INSERT", "INTO", "VALUES", "UPDATE",
            "SET", "DELETE", "CREATE", "TABLE", "VIEW", "INDEX", "DROP", "ALTER", "EXISTS", "CASE",
            "WHEN", "THEN", "ELSE", "END", "ASC", "DESC", "HAVING", "WITH", "USING", "MINUS",
            "INTERSECT"
        };

        private static readonly Color AddedColor = Color.FromArgb(232, 248, 232);
        private static readonly Color RemovedColor = Color.FromArgb(251, 232, 232);
        private static readonly Color ModifiedColor = Color.FromArgb(252, 246, 219);
        private static readonly Color ChangeIndicatorColor = Color.FromArgb(200, 120, 0);

        private readonly IntPtr owner;
        private readonly Func<ComparisonDiffViewModel, ComparisonDiffHunk, ComparisonDiffActionResult> applyHunk;
        private readonly Func<ComparisonDiffActionResult> undoLastApply;
        private readonly Func<ComparisonDiffViewModel, string, ComparisonDiffActionResult> updateLocalText;
        private readonly Func<ComparisonDiffActionResult> refreshModel;

        private readonly Panel headerPanel;
        private readonly Label headerLabel;
        private readonly Label statusLabel;
        private readonly Label leftHeaderLabel;
        private readonly Label rightHeaderLabel;
        private readonly TableLayoutPanel contentLayout;
        private readonly ScintillaEditorControl leftPane;
        private readonly Panel gutterPanel;
        private readonly ScintillaEditorControl rightPane;
        private readonly Button undoButton;
        private readonly Button refreshButton;
        private readonly Button closeButton;
        private readonly ToolTip toolTip;
        private DialogHelper.ModalDialogMouseHandler? mouseHandler;

        private readonly Dictionary<int, Button> hunkButtons = new();
        private readonly System.Windows.Forms.Timer editDebounceTimer;

        private ComparisonDiffViewModel model;
        private bool stylesConfigured;
        private bool syncingScroll;
        private bool suppressEdits;
        private bool pendingLocalEdit;

        public ComparisonDiffDialog(
            ComparisonDiffViewModel model,
            IntPtr owner,
            Func<ComparisonDiffViewModel, ComparisonDiffHunk, ComparisonDiffActionResult> applyHunk,
            Func<ComparisonDiffActionResult> undoLastApply,
            Func<ComparisonDiffViewModel, string, ComparisonDiffActionResult> updateLocalText,
            Func<ComparisonDiffActionResult> refreshModel)
        {
            this.model = model;
            this.owner = owner;
            this.applyHunk = applyHunk;
            this.undoLastApply = undoLastApply;
            this.updateLocalText = updateLocalText;
            this.refreshModel = refreshModel;

            headerPanel = new Panel();
            headerLabel = new Label();
            statusLabel = new Label();
            leftHeaderLabel = new Label();
            rightHeaderLabel = new Label();
            contentLayout = new TableLayoutPanel();
            leftPane = new ScintillaEditorControl();
            gutterPanel = new Panel();
            rightPane = new ScintillaEditorControl();
            undoButton = new Button();
            refreshButton = new Button();
            closeButton = new Button();
            toolTip = new ToolTip();
            editDebounceTimer = new System.Windows.Forms.Timer { Interval = 250 };

            InitializeComponent();
        }

        private void InitializeComponent()
        {
            SuspendLayout();
            headerPanel.SuspendLayout();
            contentLayout.SuspendLayout();

            headerPanel.BackColor = Color.FromArgb(50, 50, 60);
            headerPanel.Dock = DockStyle.Top;
            headerPanel.Height = 30;
            headerPanel.Controls.Add(headerLabel);

            headerLabel.Dock = DockStyle.Fill;
            headerLabel.ForeColor = Color.White;
            headerLabel.Font = new Font("Segoe UI", 9F, FontStyle.Regular, GraphicsUnit.Point);
            headerLabel.TextAlign = ContentAlignment.MiddleCenter;

            statusLabel.Location = new Point(16, 42);
            statusLabel.Size = new Size(1080, 20);
            statusLabel.Anchor = AnchorStyles.Top | AnchorStyles.Left | AnchorStyles.Right;

            contentLayout.Location = new Point(16, 72);
            contentLayout.Size = new Size(1080, 620);
            contentLayout.Anchor = AnchorStyles.Top | AnchorStyles.Bottom | AnchorStyles.Left | AnchorStyles.Right;
            contentLayout.ColumnCount = 3;
            contentLayout.RowCount = 2;
            contentLayout.ColumnStyles.Add(new ColumnStyle(SizeType.Percent, 50F));
            contentLayout.ColumnStyles.Add(new ColumnStyle(SizeType.Absolute, 48F));
            contentLayout.ColumnStyles.Add(new ColumnStyle(SizeType.Percent, 50F));
            contentLayout.RowStyles.Add(new RowStyle(SizeType.Absolute, 24F));
            contentLayout.RowStyles.Add(new RowStyle(SizeType.Percent, 100F));
            contentLayout.Controls.Add(leftHeaderLabel, 0, 0);
            contentLayout.Controls.Add(rightHeaderLabel, 2, 0);
            contentLayout.Controls.Add(leftPane, 0, 1);
            contentLayout.Controls.Add(gutterPanel, 1, 1);
            contentLayout.Controls.Add(rightPane, 2, 1);

            ConfigureHeaderLabel(leftHeaderLabel);
            ConfigureHeaderLabel(rightHeaderLabel);

            leftPane.Dock = DockStyle.Fill;
            rightPane.Dock = DockStyle.Fill;
            leftPane.ViewChanged += (_, _) => SyncFrom(leftPane, rightPane);
            rightPane.ViewChanged += (_, _) => SyncFrom(rightPane, leftPane);
            leftPane.TextModified += LeftPane_TextModified;
            editDebounceTimer.Tick += EditDebounceTimer_Tick;

            gutterPanel.Dock = DockStyle.Fill;
            gutterPanel.BackColor = Color.FromArgb(245, 245, 248);
            gutterPanel.Resize += (_, _) => RepositionHunkButtons();

            ConfigureButton(undoButton, "Undo", new Point(756, 708), Color.FromArgb(108, 117, 125));
            undoButton.Click += (s, e) => ApplyActionResult(undoLastApply());

            ConfigureButton(refreshButton, "Refresh", new Point(876, 708), Color.FromArgb(0, 122, 204));
            refreshButton.Click += (s, e) => ApplyActionResult(refreshModel());

            ConfigureButton(closeButton, "Close", new Point(996, 708), Color.FromArgb(100, 100, 100));
            closeButton.Click += (s, e) =>
            {
                DialogResult = DialogResult.Cancel;
                Close();
            };

            AutoScaleMode = AutoScaleMode.None;
            BackColor = Color.FromArgb(240, 240, 245);
            ClientSize = new Size(1114, 752);
            MinimumSize = new Size(920, 560);
            Controls.Add(headerPanel);
            Controls.Add(statusLabel);
            Controls.Add(contentLayout);
            Controls.Add(undoButton);
            Controls.Add(refreshButton);
            Controls.Add(closeButton);
            FormBorderStyle = FormBorderStyle.Sizable;
            MaximizeBox = true;
            MinimizeBox = true;
            ShowInTaskbar = false;
            StartPosition = FormStartPosition.Manual;
            Padding = new Padding(1);

            contentLayout.ResumeLayout(false);
            headerPanel.ResumeLayout(false);
            ResumeLayout(false);
        }

        private static void ConfigureHeaderLabel(Label label)
        {
            label.Dock = DockStyle.Fill;
            label.BackColor = Color.FromArgb(230, 230, 235);
            label.TextAlign = ContentAlignment.MiddleLeft;
            label.Padding = new Padding(8, 0, 0, 0);
            label.Font = new Font("Segoe UI", 9F, FontStyle.Bold, GraphicsUnit.Point);
        }

        private static void ConfigureButton(Button button, string text, Point location, Color back)
        {
            button.Text = text;
            button.Size = new Size(100, 30);
            button.Location = location;
            button.Anchor = AnchorStyles.Bottom | AnchorStyles.Right;
            button.BackColor = back;
            button.ForeColor = Color.White;
            button.FlatStyle = FlatStyle.Flat;
            button.FlatAppearance.BorderSize = 0;
        }

        protected override void OnShown(EventArgs e)
        {
            base.OnShown(e);

            if (owner != IntPtr.Zero)
            {
                WindowHelper.CenterFormOnWindow(this, owner);
            }

            if (Modal && owner != IntPtr.Zero)
            {
                mouseHandler = new DialogHelper.ModalDialogMouseHandler(this, headerPanel, owner);
            }

            // If hosting failed, say so plainly rather than showing two empty panes.
            if (!leftPane.IsHosted || !rightPane.IsHosted)
            {
                string detail = leftPane.HostError ?? rightPane.HostError ?? "unknown error";
                statusLabel.ForeColor = Color.Firebrick;
                statusLabel.Text = $"Could not host Scintilla diff panes: {detail}";
                return;
            }

            ConfigureStyles();
            BindModelFull(0);
            leftPane.FocusEditor();
        }

        private void ConfigureStyles()
        {
            if (stylesConfigured)
            {
                return;
            }

            foreach (var pane in new[] { leftPane, rightPane })
            {
                pane.SetReadOnly(true);
                pane.DefineLineMarker(MARKER_ADDED, AddedColor);
                pane.DefineLineMarker(MARKER_REMOVED, RemovedColor);
                pane.DefineLineMarker(MARKER_CHANGED, ModifiedColor);
                pane.DefineChangeIndicator(INDICATOR_CHANGE, ChangeIndicatorColor, 80);
                pane.DefineStyleColor(STYLE_KEYWORD, KeywordColor);
                pane.DefineStyleColor(STYLE_COMMENT, CommentColor);
                pane.DefineStyleColor(STYLE_STRING, StringColor);
            }

            stylesConfigured = true;
        }

        /// <summary>
        /// Full rebind: reloads both panes' text and re-decorates. Used for initial load, hunk
        /// apply, refresh, and undo — cases where the document text itself changed. Resets the
        /// caret (acceptable for those deliberate actions); local-edit recompute uses the surgical
        /// path in <see cref="PushLocalEdit"/> instead so typing never reloads the pane.
        /// </summary>
        private void BindModelFull(int preservedFirstVisibleLine)
        {
            suppressEdits = true;
            try
            {
                // Load the real (unpadded) document text into each pane; alignment is applied as
                // annotations afterward so the underlying text stays pristine.
                leftPane.SetText(model.LocalDisplayText);
                rightPane.SetText(model.RemoteDisplayText);

                // The local pane is editable for both PeopleCode and SQL. For SQL the panes show
                // normalized text at load/refresh; edits show as-typed and only re-normalize on the
                // next Refresh (App Designer canonicalizes SQL formatting on save regardless).
                leftPane.SetReadOnly(false);
                rightPane.SetReadOnly(true);

                ApplyHeadersAndDecorations();

                leftPane.SetFirstVisibleLine(preservedFirstVisibleLine);
                rightPane.SetFirstVisibleLine(preservedFirstVisibleLine);
                RepositionHunkButtons();
            }
            finally
            {
                suppressEdits = false;
                pendingLocalEdit = false;
            }
        }

        /// <summary>Updates headers/status and re-applies decorations + alignment to the current
        /// pane text without touching that text — safe to call after a surgical local edit.</summary>
        private void ApplyHeadersAndDecorations()
        {
            headerLabel.Text = model.Title;
            Text = model.Title;
            leftHeaderLabel.Text = $"  {model.LocalSourceName}";
            rightHeaderLabel.Text = $"  {model.RemoteSourceName}";

            statusLabel.ForeColor = SystemColors.ControlText;
            statusLabel.Text = model.HasDifferences
                ? $"{model.Hunks.Count} hunk(s) — click an arrow to pull the comparison side into the local editor."
                : "No differences.";

            ClearDecorations(leftPane);
            ClearDecorations(rightPane);
            Colorize(leftPane, model.LocalDisplayText);
            Colorize(rightPane, model.RemoteDisplayText);
            ApplyDecorationsAndAlignment();
            RebuildGutterButtons();
            RepositionHunkButtons();
        }

        /// <summary>Applies App Designer-style syntax coloring to a pane (keywords/built-in functions
        /// blue, comments green, strings red). PeopleCode uses our self-hosted lexer; SQL uses a small
        /// scanner. Byte offsets assume ASCII for SQL, which PeopleCode/SQL effectively always is.</summary>
        private void Colorize(ScintillaEditorControl pane, string text)
        {
            try
            {
                pane.ResetStyling();
                if (string.IsNullOrEmpty(text))
                {
                    return;
                }

                if (model.IsSqlNormalizedDisplay)
                {
                    ColorizeSql(pane, text);
                }
                else
                {
                    ColorizePeopleCode(pane, text);
                }
            }
            catch (Exception ex)
            {
                // Coloring is cosmetic — never let it break the diff.
                Debug.Log($"ComparisonDiffDialog.Colorize: {ex.Message}");
            }
        }

        private static void ColorizePeopleCode(ScintillaEditorControl pane, string text)
        {
            var tokens = new PeopleCodeLexer(text).TokenizeAll();
            foreach (var token in tokens)
            {
                int style = StyleForPeopleCodeToken(token);
                if (style == 0)
                {
                    continue;
                }

                int start = token.SourceSpan.Start.ByteIndex;
                pane.StyleRange(start, token.SourceSpan.End.ByteIndex - start, style);
            }
        }

        private static int StyleForPeopleCodeToken(Token token)
        {
            TokenType type = token.Type;
            if (type.IsCommentType())
            {
                return STYLE_COMMENT;
            }

            if (type == TokenType.StringLiteral ||
                type == TokenType.InterpStringStart ||
                type == TokenType.InterpStringMid ||
                type == TokenType.InterpStringEnd ||
                type == TokenType.InterpStringUnterminated)
            {
                return STYLE_STRING;
            }

            if (type.IsKeyword())
            {
                return STYLE_KEYWORD;
            }

            // Built-in functions (GetSQL, CreateRecord, ...) render in the same blue as keywords.
            // User methods/variables are not in the type database and stay default.
            if (type == TokenType.GenericId && PeopleCodeTypeDatabase.GetFunction(token.Text) != null)
            {
                return STYLE_KEYWORD;
            }

            return 0;
        }

        private static void ColorizeSql(ScintillaEditorControl pane, string text)
        {
            int i = 0;
            int n = text.Length;
            while (i < n)
            {
                char c = text[i];

                // Line comment: -- to end of line
                if (c == '-' && i + 1 < n && text[i + 1] == '-')
                {
                    int start = i;
                    i += 2;
                    while (i < n && text[i] != '\n') i++;
                    pane.StyleRange(start, i - start, STYLE_COMMENT);
                    continue;
                }

                // Block comment: /* ... */
                if (c == '/' && i + 1 < n && text[i + 1] == '*')
                {
                    int start = i;
                    i += 2;
                    while (i + 1 < n && !(text[i] == '*' && text[i + 1] == '/')) i++;
                    i = i + 1 < n ? i + 2 : n;
                    pane.StyleRange(start, i - start, STYLE_COMMENT);
                    continue;
                }

                // String literal: '...' with '' as an escaped quote
                if (c == '\'')
                {
                    int start = i;
                    i++;
                    while (i < n)
                    {
                        if (text[i] == '\'')
                        {
                            if (i + 1 < n && text[i + 1] == '\'') { i += 2; continue; }
                            i++;
                            break;
                        }
                        i++;
                    }
                    pane.StyleRange(start, i - start, STYLE_STRING);
                    continue;
                }

                // Word: keyword if it matches the SQL keyword set
                if (char.IsLetter(c) || c == '_')
                {
                    int start = i;
                    while (i < n && (char.IsLetterOrDigit(text[i]) || text[i] == '_' || text[i] == '$' || text[i] == '#')) i++;
                    if (SqlKeywords.Contains(text.Substring(start, i - start)))
                    {
                        pane.StyleRange(start, i - start, STYLE_KEYWORD);
                    }
                    continue;
                }

                i++;
            }
        }

        private static void ClearDecorations(ScintillaEditorControl pane)
        {
            pane.ClearAnnotations();
            pane.ClearLineMarkers(MARKER_ADDED);
            pane.ClearLineMarkers(MARKER_REMOVED);
            pane.ClearLineMarkers(MARKER_CHANGED);
            pane.ClearIndicatorRange(INDICATOR_CHANGE, 0, pane.TextLength);
        }

        private void LeftPane_TextModified(object? sender, EventArgs e)
        {
            // Programmatic SetText (during a full rebind) is wrapped in suppressEdits; ignore it.
            if (suppressEdits)
            {
                return;
            }

            pendingLocalEdit = true;
            editDebounceTimer.Stop();
            editDebounceTimer.Start();
        }

        private void EditDebounceTimer_Tick(object? sender, EventArgs e)
        {
            editDebounceTimer.Stop();
            if (suppressEdits || !leftPane.IsHosted)
            {
                return;
            }

            PushLocalEdit(redecorate: true);
        }

        /// <summary>
        /// Pushes the local pane's current text to the real App Designer editor and recomputes the
        /// diff. When <paramref name="redecorate"/> is true, re-applies decorations in place without
        /// reloading the pane (preserving caret/viewport), falling back to a full rebind only if the
        /// round-tripped text drifted from the pane (e.g. EOL normalization).
        /// </summary>
        private void PushLocalEdit(bool redecorate)
        {
            pendingLocalEdit = false;

            string leftText = leftPane.GetText();
            var result = updateLocalText(model, leftText);
            if (result.UpdatedModel == null)
            {
                return;
            }

            model = result.UpdatedModel;

            if (!redecorate)
            {
                return;
            }

            if (string.Equals(leftPane.GetText(), model.LocalDisplayText, StringComparison.Ordinal))
            {
                ApplyHeadersAndDecorations();
                rightPane.SetFirstVisibleLine(leftPane.GetFirstVisibleLine());
            }
            else
            {
                BindModelFull(leftPane.GetFirstVisibleLine());
            }
        }

        /// <summary>
        /// Walks the aligned row model once to apply per-line background markers, intra-line change
        /// indicators, and blank-line alignment annotations to each pane.
        /// </summary>
        private void ApplyDecorationsAndAlignment()
        {
            int leftLine = 0;
            int rightLine = 0;
            var leftFill = new Dictionary<int, int>();
            var rightFill = new Dictionary<int, int>();

            foreach (var row in model.Rows)
            {
                bool leftReal = row.LeftChangeType != ChangeType.Imaginary;
                bool rightReal = row.RightChangeType != ChangeType.Imaginary;

                if (leftReal)
                {
                    int marker = MarkerForChange(row.LeftChangeType);
                    if (marker >= 0)
                    {
                        leftPane.AddLineMarker(marker, leftLine);
                    }

                    if (row.LeftChangeType == ChangeType.Modified)
                    {
                        FillChangeIndicator(leftPane, leftLine, row.LeftText, row.RightText);
                    }
                }

                if (rightReal)
                {
                    int marker = MarkerForChange(row.RightChangeType);
                    if (marker >= 0)
                    {
                        rightPane.AddLineMarker(marker, rightLine);
                    }

                    if (row.RightChangeType == ChangeType.Modified)
                    {
                        FillChangeIndicator(rightPane, rightLine, row.RightText, row.LeftText);
                    }
                }

                // A padding (imaginary) row on one side means the other side has a line here that
                // this side lacks; add a filler line after this side's previous real line to align.
                if (!leftReal && rightReal)
                {
                    int anchor = Math.Max(0, leftLine - 1);
                    leftFill[anchor] = leftFill.GetValueOrDefault(anchor) + 1;
                }
                else if (leftReal && !rightReal)
                {
                    int anchor = Math.Max(0, rightLine - 1);
                    rightFill[anchor] = rightFill.GetValueOrDefault(anchor) + 1;
                }

                if (leftReal)
                {
                    leftLine++;
                }

                if (rightReal)
                {
                    rightLine++;
                }
            }

            foreach (var (line, count) in leftFill)
            {
                leftPane.SetAlignmentAnnotation(line, count);
            }

            foreach (var (line, count) in rightFill)
            {
                rightPane.SetAlignmentAnnotation(line, count);
            }
        }

        private static void FillChangeIndicator(ScintillaEditorControl pane, int docLine, string lineText, string otherText)
        {
            var span = FindChangedSpan(lineText ?? string.Empty, otherText ?? string.Empty);
            if (span.Length <= 0)
            {
                return;
            }

            // Scintilla positions are UTF-8 byte offsets; convert the char span accordingly.
            int lineStart = pane.PositionFromLine(docLine);
            int byteStart = Encoding.UTF8.GetByteCount((lineText ?? string.Empty).Substring(0, span.Start));
            int byteLen = Encoding.UTF8.GetByteCount((lineText ?? string.Empty).Substring(span.Start, span.Length));
            pane.FillIndicatorRange(INDICATOR_CHANGE, lineStart + byteStart, byteLen);
        }

        private static int MarkerForChange(ChangeType changeType) => changeType switch
        {
            ChangeType.Inserted => MARKER_ADDED,
            ChangeType.Deleted => MARKER_REMOVED,
            ChangeType.Modified => MARKER_CHANGED,
            _ => -1
        };

        private static (int Start, int Length) FindChangedSpan(string left, string right)
        {
            int maxPrefix = Math.Min(left.Length, right.Length);
            int prefix = 0;
            while (prefix < maxPrefix && left[prefix] == right[prefix])
            {
                prefix++;
            }

            int leftSuffixIndex = left.Length - 1;
            int rightSuffixIndex = right.Length - 1;
            while (leftSuffixIndex >= prefix &&
                   rightSuffixIndex >= prefix &&
                   left[leftSuffixIndex] == right[rightSuffixIndex])
            {
                leftSuffixIndex--;
                rightSuffixIndex--;
            }

            int changedLength = Math.Max(0, (leftSuffixIndex - prefix) + 1);
            return (prefix, changedLength);
        }

        private void RebuildGutterButtons()
        {
            foreach (var button in hunkButtons.Values)
            {
                button.Dispose();
            }

            hunkButtons.Clear();
            gutterPanel.Controls.Clear();

            foreach (var hunk in model.Hunks)
            {
                Button button = new()
                {
                    Width = 34,
                    Height = 22,
                    FlatStyle = FlatStyle.Flat,
                    BackColor = Color.FromArgb(0, 122, 204),
                    ForeColor = Color.White,
                    Text = "←",
                    Font = new Font("Segoe UI Symbol", 11F, FontStyle.Bold, GraphicsUnit.Point),
                    Tag = hunk.Id,
                    TabStop = false
                };

                button.FlatAppearance.BorderSize = 0;
                button.Click += GutterButton_Click;
                toolTip.SetToolTip(button, $"Pull hunk {hunk.Id} into local editor");

                hunkButtons[hunk.Id] = button;
                gutterPanel.Controls.Add(button);
            }
        }

        private void RepositionHunkButtons()
        {
            if (!leftPane.IsHosted)
            {
                return;
            }

            foreach (var hunk in model.Hunks)
            {
                if (!hunkButtons.TryGetValue(hunk.Id, out var button))
                {
                    continue;
                }

                // Anchor the arrow to the side that actually holds the changed block: the remote
                // (right) pane when the hunk has remote lines — which is both where the pulled
                // content lives and where a pure insertion renders (the local side is just a blank
                // alignment gap there). Fall back to the local pane for pure deletions.
                bool remoteHasContent = hunk.RemoteEndLine > hunk.RemoteStartLine;
                int y = remoteHasContent
                    ? rightPane.PointYFromLine(hunk.RemoteStartLine)
                    : leftPane.PointYFromLine(hunk.LocalStartLine);

                button.Left = Math.Max(7, (gutterPanel.ClientSize.Width - button.Width) / 2);
                button.Top = Math.Max(2, y + 1);
                button.Visible = y >= 0 && y < gutterPanel.ClientSize.Height - button.Height;
            }
        }

        private void GutterButton_Click(object? sender, EventArgs e)
        {
            if (sender is not Button button || button.Tag is not int hunkId)
            {
                return;
            }

            var hunk = model.Hunks.FirstOrDefault(current => current.Id == hunkId);
            if (hunk == null)
            {
                return;
            }

            ApplyActionResult(applyHunk(model, hunk));
        }

        private void SyncFrom(ScintillaEditorControl source, ScintillaEditorControl target)
        {
            if (syncingScroll || !source.IsHosted || !target.IsHosted)
            {
                return;
            }

            syncingScroll = true;
            try
            {
                target.SetFirstVisibleLine(source.GetFirstVisibleLine());
                RepositionHunkButtons();
            }
            finally
            {
                syncingScroll = false;
            }
        }

        private void ApplyActionResult(ComparisonDiffActionResult result)
        {
            editDebounceTimer.Stop();
            int preservedLine = leftPane.IsHosted ? leftPane.GetFirstVisibleLine() : 0;

            if (!string.IsNullOrWhiteSpace(result.Message))
            {
                new MessageBoxDialog(result.Message, result.MessageTitle, MessageBoxButtons.OK, owner)
                    .ShowDialog(new WindowWrapper(owner));
            }

            if (result.UpdatedModel != null)
            {
                model = result.UpdatedModel;
                BindModelFull(preservedLine);
            }

            // Return focus to the editable pane so Enter inserts a newline instead of re-clicking
            // the button that was just pressed. Deferred so it runs after the click is fully
            // processed (otherwise WinForms restores focus to the button afterward).
            if (leftPane.IsHosted)
            {
                BeginInvoke(new Action(() => leftPane.FocusEditor()));
            }
        }

        protected override bool ProcessCmdKey(ref Message msg, Keys keyData)
        {
            if (keyData == (Keys.Control | Keys.Z))
            {
                ApplyActionResult(undoLastApply());
                return true;
            }

            return base.ProcessCmdKey(ref msg, keyData);
        }

        protected override bool ProcessDialogKey(Keys keyData)
        {
            if (keyData == Keys.Escape)
            {
                DialogResult = DialogResult.Cancel;
                Close();
                return true;
            }

            return base.ProcessDialogKey(keyData);
        }

        protected override void OnFormClosed(FormClosedEventArgs e)
        {
            base.OnFormClosed(e);

            editDebounceTimer.Stop();

            // Flush any typing that hasn't been pushed to the editor yet (closing within the
            // debounce window). Don't redecorate — the dialog is going away.
            if (pendingLocalEdit && leftPane.IsHosted)
            {
                PushLocalEdit(redecorate: false);
            }

            editDebounceTimer.Dispose();
            mouseHandler?.Dispose();
            mouseHandler = null;
            foreach (var button in hunkButtons.Values)
            {
                button.Dispose();
            }
            hunkButtons.Clear();
            toolTip.Dispose();
        }
    }
}
