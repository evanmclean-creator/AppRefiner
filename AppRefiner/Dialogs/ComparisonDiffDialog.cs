using AppRefiner.Services;
using DiffPlex.DiffBuilder.Model;
using System.Runtime.InteropServices;

namespace AppRefiner.Dialogs
{
    /// <summary>
    /// Side-by-side comparison dialog with synchronized panes and per-hunk apply actions.
    /// </summary>
    public sealed class ComparisonDiffDialog : Form
    {
        private const int EM_GETFIRSTVISIBLELINE = 0x00CE;
        private const int EM_LINESCROLL = 0x00B6;

        [DllImport("user32.dll", CharSet = CharSet.Auto)]
        private static extern IntPtr SendMessage(IntPtr hWnd, int msg, IntPtr wParam, IntPtr lParam);

        private static readonly Color AddedColor = Color.FromArgb(232, 248, 232);
        private static readonly Color AddedEmphasisColor = Color.FromArgb(205, 239, 205);
        private static readonly Color RemovedColor = Color.FromArgb(251, 232, 232);
        private static readonly Color RemovedEmphasisColor = Color.FromArgb(245, 210, 210);
        private static readonly Color ModifiedColor = Color.FromArgb(252, 246, 219);
        private static readonly Color ModifiedEmphasisColor = Color.FromArgb(248, 232, 180);
        private static readonly Color ImaginaryColor = Color.FromArgb(245, 245, 245);
        private static readonly Color ChangedTextColor = Color.FromArgb(160, 38, 38);

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
        private readonly RichTextBox leftTextBox;
        private readonly Panel gutterPanel;
        private readonly RichTextBox rightTextBox;
        private readonly Button undoButton;
        private readonly Button refreshButton;
        private readonly Button closeButton;
        private readonly ToolTip toolTip;
        private DialogHelper.ModalDialogMouseHandler? mouseHandler;
        private readonly System.Windows.Forms.Timer localEditDebounceTimer;

        private readonly Dictionary<int, Button> hunkButtons = new();
        private readonly List<int> rowStartIndices = new();

        private ComparisonDiffViewModel model;
        private bool syncingScroll;
        private bool rebindingModel;
        private bool suppressLocalEditEvents;

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
            leftTextBox = new RichTextBox();
            gutterPanel = new Panel();
            rightTextBox = new RichTextBox();
            undoButton = new Button();
            refreshButton = new Button();
            closeButton = new Button();
            toolTip = new ToolTip();
            localEditDebounceTimer = new System.Windows.Forms.Timer { Interval = 250 };

            InitializeComponent();
            BindModel(0);
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
            contentLayout.ColumnStyles.Add(new ColumnStyle(SizeType.Percent, 48F));
            contentLayout.ColumnStyles.Add(new ColumnStyle(SizeType.Absolute, 48F));
            contentLayout.ColumnStyles.Add(new ColumnStyle(SizeType.Percent, 48F));
            contentLayout.RowStyles.Add(new RowStyle(SizeType.Absolute, 24F));
            contentLayout.RowStyles.Add(new RowStyle(SizeType.Percent, 100F));
            contentLayout.Controls.Add(leftHeaderLabel, 0, 0);
            contentLayout.Controls.Add(rightHeaderLabel, 2, 0);
            contentLayout.Controls.Add(leftTextBox, 0, 1);
            contentLayout.Controls.Add(gutterPanel, 1, 1);
            contentLayout.Controls.Add(rightTextBox, 2, 1);

            leftHeaderLabel.Dock = DockStyle.Fill;
            leftHeaderLabel.BackColor = Color.FromArgb(230, 230, 235);
            leftHeaderLabel.TextAlign = ContentAlignment.MiddleLeft;
            leftHeaderLabel.Padding = new Padding(8, 0, 0, 0);
            leftHeaderLabel.Font = new Font("Segoe UI", 9F, FontStyle.Bold, GraphicsUnit.Point);

            rightHeaderLabel.Dock = DockStyle.Fill;
            rightHeaderLabel.BackColor = Color.FromArgb(230, 230, 235);
            rightHeaderLabel.TextAlign = ContentAlignment.MiddleLeft;
            rightHeaderLabel.Padding = new Padding(8, 0, 0, 0);
            rightHeaderLabel.Font = new Font("Segoe UI", 9F, FontStyle.Bold, GraphicsUnit.Point);

            ConfigureTextPane(leftTextBox);
            ConfigureTextPane(rightTextBox);
            leftTextBox.ReadOnly = false;
            rightTextBox.ReadOnly = true;
            leftTextBox.VScroll += TextBox_VScroll;
            rightTextBox.VScroll += TextBox_VScroll;
            leftTextBox.MouseWheel += TextBox_MouseWheel;
            rightTextBox.MouseWheel += TextBox_MouseWheel;
            leftTextBox.TextChanged += LeftTextBox_TextChanged;
            leftTextBox.Resize += (_, _) => RepositionHunkButtons();
            rightTextBox.Resize += (_, _) => RepositionHunkButtons();
            localEditDebounceTimer.Tick += LocalEditDebounceTimer_Tick;

            gutterPanel.Dock = DockStyle.Fill;
            gutterPanel.BackColor = Color.FromArgb(245, 245, 248);
            gutterPanel.Resize += (_, _) => RepositionHunkButtons();

            undoButton.Text = "Undo";
            undoButton.Size = new Size(100, 30);
            undoButton.Location = new Point(756, 708);
            undoButton.Anchor = AnchorStyles.Bottom | AnchorStyles.Right;
            undoButton.BackColor = Color.FromArgb(108, 117, 125);
            undoButton.ForeColor = Color.White;
            undoButton.FlatStyle = FlatStyle.Flat;
            undoButton.FlatAppearance.BorderSize = 0;
            undoButton.Click += UndoButton_Click;

            refreshButton.Text = "Refresh";
            refreshButton.Size = new Size(100, 30);
            refreshButton.Location = new Point(876, 708);
            refreshButton.Anchor = AnchorStyles.Bottom | AnchorStyles.Right;
            refreshButton.BackColor = Color.FromArgb(0, 122, 204);
            refreshButton.ForeColor = Color.White;
            refreshButton.FlatStyle = FlatStyle.Flat;
            refreshButton.FlatAppearance.BorderSize = 0;
            refreshButton.Click += RefreshButton_Click;

            closeButton.Text = "Close";
            closeButton.Size = new Size(100, 30);
            closeButton.Location = new Point(996, 708);
            closeButton.Anchor = AnchorStyles.Bottom | AnchorStyles.Right;
            closeButton.BackColor = Color.FromArgb(100, 100, 100);
            closeButton.ForeColor = Color.White;
            closeButton.FlatStyle = FlatStyle.Flat;
            closeButton.FlatAppearance.BorderSize = 0;
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

        private static void ConfigureTextPane(RichTextBox textBox)
        {
            textBox.Dock = DockStyle.Fill;
            textBox.BorderStyle = BorderStyle.FixedSingle;
            textBox.ReadOnly = true;
            textBox.Font = new Font("Consolas", 10F, FontStyle.Regular, GraphicsUnit.Point);
            textBox.WordWrap = false;
            textBox.ScrollBars = RichTextBoxScrollBars.Both;
            textBox.BackColor = Color.White;
            textBox.HideSelection = false;
            textBox.DetectUrls = false;
            textBox.Multiline = true;
            textBox.ShortcutsEnabled = true;
        }

        private void BindModel(int preservedFirstVisibleLine)
        {
            rebindingModel = true;
            suppressLocalEditEvents = true;
            try
            {
                headerLabel.Text = model.Title;
                Text = model.Title;
                leftHeaderLabel.Text = $"  {model.LocalSourceName}";
                rightHeaderLabel.Text = $"  {model.RemoteSourceName}";

                statusLabel.Text = model.HasDifferences
                    ? $"{model.Hunks.Count} hunk(s) available"
                    : "No changes detected.";

                if (model.HasDifferences && model.UsesWholeDocumentApply)
                {
                    statusLabel.Text += "  SQL apply uses the full comparison definition.";
                }

                rowStartIndices.Clear();
                leftTextBox.Clear();
                rightTextBox.Clear();

                leftTextBox.SuspendLayout();
                rightTextBox.SuspendLayout();

                for (int i = 0; i < model.Rows.Count; i++)
                {
                    var row = model.Rows[i];
                    rowStartIndices.Add(leftTextBox.TextLength);

                    AppendLine(leftTextBox, row.LeftText, row.LeftChangeType, row.LeftText, row.RightText);
                    AppendLine(rightTextBox, row.RightText, row.RightChangeType, row.RightText, row.LeftText);
                }

                leftTextBox.SelectionLength = 0;
                rightTextBox.SelectionLength = 0;
                leftTextBox.SelectionStart = 0;
                rightTextBox.SelectionStart = 0;

                leftTextBox.ResumeLayout();
                rightTextBox.ResumeLayout();

                RebuildGutterButtons();
                RestoreVisibleLine(preservedFirstVisibleLine);
            }
            finally
            {
                suppressLocalEditEvents = false;
                rebindingModel = false;
            }

            RepositionHunkButtons();
        }

        private void AppendLine(RichTextBox textBox, string text, ChangeType changeType, string primaryText, string comparisonText)
        {
            string lineText = text ?? string.Empty;
            int lineStart = textBox.TextLength;
            textBox.AppendText(lineText + Environment.NewLine);
            int lineLength = lineText.Length;
            int lineEnd = lineStart + lineLength;

            ApplyWholeLineStyle(textBox, lineStart, lineLength, changeType);

            if (lineLength > 0 && changeType == ChangeType.Modified)
            {
                var changedSpan = FindChangedSpan(primaryText ?? string.Empty, comparisonText ?? string.Empty);
                if (changedSpan.Length > 0)
                {
                    textBox.Select(lineStart + changedSpan.Start, changedSpan.Length);
                    textBox.SelectionBackColor = ModifiedEmphasisColor;
                    textBox.SelectionColor = ChangedTextColor;
                    textBox.SelectionFont = new Font(textBox.Font, FontStyle.Bold);
                }
            }

            textBox.Select(lineEnd, 0);
            textBox.SelectionBackColor = textBox.BackColor;
            textBox.SelectionColor = Color.Black;
            textBox.SelectionFont = textBox.Font;
        }

        private static void ApplyWholeLineStyle(RichTextBox textBox, int start, int length, ChangeType changeType)
        {
            textBox.Select(start, Math.Max(0, length));
            textBox.SelectionColor = Color.Black;
            textBox.SelectionFont = textBox.Font;

            switch (changeType)
            {
                case ChangeType.Inserted:
                    textBox.SelectionBackColor = AddedColor;
                    break;
                case ChangeType.Deleted:
                    textBox.SelectionBackColor = RemovedColor;
                    break;
                case ChangeType.Modified:
                    textBox.SelectionBackColor = ModifiedColor;
                    break;
                case ChangeType.Imaginary:
                    textBox.SelectionBackColor = ImaginaryColor;
                    break;
                default:
                    textBox.SelectionBackColor = Color.White;
                    break;
            }
        }

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
                if (model.UsesWholeDocumentApply && hunk.Id > 1)
                {
                    continue;
                }

                Button button = new()
                {
                    Width = 34,
                    Height = 24,
                    FlatStyle = FlatStyle.Flat,
                    BackColor = Color.FromArgb(0, 122, 204),
                    ForeColor = Color.White,
                    Text = "\u2190",
                    Font = new Font("Segoe UI Symbol", 11F, FontStyle.Bold, GraphicsUnit.Point),
                    Tag = hunk.Id,
                    TabStop = false
                };

                button.FlatAppearance.BorderSize = 0;
                button.Click += GutterButton_Click;
                toolTip.SetToolTip(button, model.UsesWholeDocumentApply ? "Apply full comparison definition to local editor" : $"Apply hunk {hunk.Id} to local editor");

                hunkButtons[hunk.Id] = button;
                gutterPanel.Controls.Add(button);
            }
        }

        private void RepositionHunkButtons()
        {
            if (rowStartIndices.Count == 0 || leftTextBox.IsDisposed || !leftTextBox.IsHandleCreated)
            {
                return;
            }

            foreach (var hunk in model.Hunks)
            {
                if (!hunkButtons.TryGetValue(hunk.Id, out var button))
                {
                    continue;
                }

                int rowIndex = Math.Clamp(hunk.StartRowIndex, 0, rowStartIndices.Count - 1);
                int charIndex = rowStartIndices[rowIndex];
                Point textPoint = leftTextBox.GetPositionFromCharIndex(charIndex);
                int y = textPoint.Y + 1;

                button.Left = Math.Max(7, (gutterPanel.ClientSize.Width - button.Width) / 2);
                button.Top = Math.Max(2, y);
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

            int preservedLine = GetFirstVisibleLine(leftTextBox);
            ApplyActionResult(applyHunk(model, hunk), preservedLine);
        }

        private void LeftTextBox_TextChanged(object? sender, EventArgs e)
        {
            if (rebindingModel || suppressLocalEditEvents)
            {
                return;
            }

            localEditDebounceTimer.Stop();
            localEditDebounceTimer.Start();
        }

        private void LocalEditDebounceTimer_Tick(object? sender, EventArgs e)
        {
            localEditDebounceTimer.Stop();

            if (rebindingModel || suppressLocalEditEvents)
            {
                return;
            }

            string currentLocalText = leftTextBox.Text.Replace("\r\n", "\n");
            string modelLocalText = model.LocalDisplayText.Replace("\r\n", "\n");
            if (string.Equals(currentLocalText, modelLocalText, StringComparison.Ordinal))
            {
                return;
            }

            int preservedLine = GetFirstVisibleLine(leftTextBox);
            ApplyActionResult(updateLocalText(model, currentLocalText), preservedLine);
        }

        private void UndoButton_Click(object? sender, EventArgs e)
        {
            localEditDebounceTimer.Stop();
            int preservedLine = GetFirstVisibleLine(leftTextBox);
            ApplyActionResult(undoLastApply(), preservedLine);
        }

        private void RefreshButton_Click(object? sender, EventArgs e)
        {
            localEditDebounceTimer.Stop();
            int preservedLine = GetFirstVisibleLine(leftTextBox);
            ApplyActionResult(refreshModel(), preservedLine);
        }

        private void ApplyActionResult(ComparisonDiffActionResult result, int preservedFirstVisibleLine)
        {
            if (!string.IsNullOrWhiteSpace(result.Message))
            {
                new MessageBoxDialog(result.Message, result.MessageTitle, MessageBoxButtons.OK, owner)
                    .ShowDialog(new WindowWrapper(owner));
            }

            if (result.UpdatedModel != null)
            {
                model = result.UpdatedModel;
                BindModel(preservedFirstVisibleLine);
            }
        }

        private void TextBox_VScroll(object? sender, EventArgs e)
        {
            if (rebindingModel || syncingScroll || sender is not RichTextBox source)
            {
                return;
            }

            SyncOtherPane(source);
        }

        private void TextBox_MouseWheel(object? sender, MouseEventArgs e)
        {
            if (rebindingModel || syncingScroll || sender is not RichTextBox source)
            {
                return;
            }

            BeginInvoke(new Action(() =>
            {
                if (!IsDisposed)
                {
                    SyncOtherPane(source);
                }
            }));
        }

        private void SyncOtherPane(RichTextBox source)
        {
            RichTextBox target = ReferenceEquals(source, leftTextBox) ? rightTextBox : leftTextBox;
            int visibleLine = GetFirstVisibleLine(source);
            syncingScroll = true;
            try
            {
                SetFirstVisibleLine(target, visibleLine);
                RepositionHunkButtons();
            }
            finally
            {
                syncingScroll = false;
            }
        }

        private void RestoreVisibleLine(int visibleLine)
        {
            syncingScroll = true;
            try
            {
                SetFirstVisibleLine(leftTextBox, visibleLine);
                SetFirstVisibleLine(rightTextBox, visibleLine);
            }
            finally
            {
                syncingScroll = false;
            }
        }

        private static int GetFirstVisibleLine(RichTextBox textBox)
        {
            return (int)SendMessage(textBox.Handle, EM_GETFIRSTVISIBLELINE, IntPtr.Zero, IntPtr.Zero);
        }

        private static void SetFirstVisibleLine(RichTextBox textBox, int targetLine)
        {
            int current = GetFirstVisibleLine(textBox);
            int delta = targetLine - current;
            if (delta != 0)
            {
                SendMessage(textBox.Handle, EM_LINESCROLL, IntPtr.Zero, new IntPtr(delta));
            }
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

            RepositionHunkButtons();
        }

        protected override bool ProcessCmdKey(ref Message msg, Keys keyData)
        {
            if (keyData == (Keys.Control | Keys.Z))
            {
                UndoButton_Click(this, EventArgs.Empty);
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
            localEditDebounceTimer.Stop();
            localEditDebounceTimer.Dispose();
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
