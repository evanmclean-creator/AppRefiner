using AppRefiner.Database.Models;
using AppRefiner.Dialogs;
using DiffPlex;
using DiffPlex.DiffBuilder;
using DiffPlex.DiffBuilder.Model;

namespace AppRefiner.Services
{
    public sealed class ComparisonDiffRow
    {
        public string LeftText { get; init; } = string.Empty;
        public string RightText { get; init; } = string.Empty;
        public ChangeType LeftChangeType { get; init; }
        public ChangeType RightChangeType { get; init; }
        public int? HunkId { get; init; }
        public bool ShowPullButton { get; init; }
        public string ActionText { get; init; } = string.Empty;
    }

    public sealed class ComparisonDiffHunk
    {
        public int Id { get; init; }
        public int StartRowIndex { get; init; }
        public int EndRowIndex { get; init; }
        public int LocalStartLine { get; init; }
        public int LocalEndLine { get; init; }
        public int RemoteStartLine { get; init; }
        public int RemoteEndLine { get; init; }
        public int LocalDisplayStartIndex { get; init; }
        public int LocalDisplayEndIndex { get; init; }
        public int RemoteDisplayStartIndex { get; init; }
        public int RemoteDisplayEndIndex { get; init; }
        public int LocalRawStartIndex { get; init; }
        public int LocalRawEndIndex { get; init; }
        public string RemoteDisplayText { get; init; } = string.Empty;
        public string RemoteRawText { get; init; } = string.Empty;
        public string Summary { get; init; } = string.Empty;
        public bool AppliesWholeDocument { get; init; }
    }

    public sealed class ComparisonDiffViewModel
    {
        public string Title { get; init; } = string.Empty;
        public string LocalSourceName { get; init; } = string.Empty;
        public string RemoteSourceName { get; init; } = string.Empty;
        public string LocalRawText { get; init; } = string.Empty;
        public string RemoteRawText { get; init; } = string.Empty;
        public string LocalDisplayText { get; init; } = string.Empty;
        public string RemoteDisplayText { get; init; } = string.Empty;
        public OpenTarget? OpenTarget { get; init; }
        public bool IsSqlNormalizedDisplay { get; init; }
        public bool HasDifferences { get; init; }
        public List<ComparisonDiffRow> Rows { get; init; } = new();
        public List<ComparisonDiffHunk> Hunks { get; init; } = new();
        public OpenTargetType OpenTargetType { get; init; }
        public bool UsesWholeDocumentApply => IsSqlNormalizedDisplay;
    }

    public enum ComparisonDiffActionStatus
    {
        Applied,
        Refreshed,
        NoChange,
        Error
    }

    public sealed class ComparisonDiffActionResult
    {
        public ComparisonDiffActionStatus Status { get; init; }
        public ComparisonDiffViewModel? UpdatedModel { get; init; }
        public string? Message { get; init; }
        public string MessageTitle { get; init; } = "Comparison";
        public MessageBoxIcon MessageIcon { get; init; } = MessageBoxIcon.Information;
    }

    internal sealed class TextLineIndex
    {
        public required int[] LineStarts { get; init; }
        public required int LineCount { get; init; }
        public required int TextLength { get; init; }
    }

    /// <summary>
    /// Fetches, displays, and applies comparison diffs for the active editor against a connected comparison database.
    /// </summary>
    public sealed class EditorComparisonService
    {
        public void ShowDiffForEditor(ScintillaEditor editor, ComparisonConnectionSession comparisonSession, IntPtr ownerHandle)
        {
            var initialResult = BuildViewModel(editor, comparisonSession);
            if (initialResult.Model == null)
            {
                ShowMessage(initialResult.Message ?? "Unable to create comparison diff.", initialResult.Title, initialResult.Icon, ownerHandle);
                return;
            }

            using var dialog = new ComparisonDiffDialog(
                initialResult.Model,
                ownerHandle,
                (viewModel, hunk) => ApplyHunk(editor, comparisonSession, viewModel, hunk),
                () => UndoLastApply(editor, comparisonSession),
                (viewModel, text) => UpdateLocalText(editor, viewModel, text),
                () => Refresh(editor, comparisonSession));

            dialog.ShowDialog(new WindowWrapper(ownerHandle));
        }

        private ComparisonDiffActionResult Refresh(ScintillaEditor editor, ComparisonConnectionSession comparisonSession)
        {
            var refreshResult = BuildViewModel(editor, comparisonSession);
            if (refreshResult.Model == null)
            {
                return new ComparisonDiffActionResult
                {
                    Status = ComparisonDiffActionStatus.Error,
                    Message = refreshResult.Message,
                    MessageTitle = refreshResult.Title,
                    MessageIcon = refreshResult.Icon
                };
            }

            return new ComparisonDiffActionResult
            {
                Status = ComparisonDiffActionStatus.Refreshed,
                UpdatedModel = refreshResult.Model
            };
        }

        private ComparisonDiffActionResult UndoLastApply(ScintillaEditor editor, ComparisonConnectionSession comparisonSession)
        {
            ScintillaManager.Undo(editor);
            editor.ContentString = ScintillaManager.GetScintillaText(editor);

            var refreshResult = BuildViewModel(editor, comparisonSession);
            if (refreshResult.Model == null)
            {
                return new ComparisonDiffActionResult
                {
                    Status = ComparisonDiffActionStatus.Error,
                    Message = refreshResult.Message,
                    MessageTitle = refreshResult.Title,
                    MessageIcon = refreshResult.Icon
                };
            }

            return new ComparisonDiffActionResult
            {
                Status = ComparisonDiffActionStatus.Refreshed,
                UpdatedModel = refreshResult.Model
            };
        }

        private ComparisonDiffActionResult UpdateLocalText(ScintillaEditor editor, ComparisonDiffViewModel currentViewModel, string newText)
        {
            string currentText = ScintillaManager.GetScintillaText(editor) ?? string.Empty;
            if (string.Equals(currentText, newText, StringComparison.Ordinal))
            {
                return RebuildFromTexts(
                    editor.AppDesignerProcess.DBName ?? "Current",
                    currentViewModel.RemoteSourceName,
                    currentViewModel.OpenTargetType,
                    editor.Caption,
                    newText,
                    currentViewModel.RemoteRawText,
                    currentViewModel.OpenTarget);
            }

            try
            {
                ScintillaManager.BeginUndoAction(editor);
                ScintillaManager.ReplaceTextRange(editor, 0, currentText.Length, newText);
            }
            finally
            {
                ScintillaManager.EndUndoAction(editor);
            }

            editor.ContentString = ScintillaManager.GetScintillaText(editor);
            return RebuildFromTexts(
                editor.AppDesignerProcess.DBName ?? "Current",
                currentViewModel.RemoteSourceName,
                currentViewModel.OpenTargetType,
                editor.Caption,
                editor.ContentString ?? newText,
                currentViewModel.RemoteRawText,
                currentViewModel.OpenTarget);
        }

        private ComparisonDiffActionResult ApplyHunk(
            ScintillaEditor editor,
            ComparisonConnectionSession comparisonSession,
            ComparisonDiffViewModel viewModel,
            ComparisonDiffHunk hunk)
        {
            var currentText = ScintillaManager.GetScintillaText(editor) ?? string.Empty;

            bool stale = viewModel.IsSqlNormalizedDisplay
                ? !string.Equals(ScintillaManager.NormalizeSqlForDiff(currentText), viewModel.LocalDisplayText, StringComparison.Ordinal)
                : !string.Equals(currentText, viewModel.LocalRawText, StringComparison.Ordinal);

            if (stale)
            {
                var refreshed = Refresh(editor, comparisonSession);
                return new ComparisonDiffActionResult
                {
                    Status = ComparisonDiffActionStatus.Refreshed,
                    UpdatedModel = refreshed.UpdatedModel,
                    Message = "The editor changed since this diff was built. The comparison has been refreshed before applying anything.",
                    MessageTitle = "Diff Refreshed",
                    MessageIcon = MessageBoxIcon.Information
                };
            }

            try
            {
                ScintillaManager.BeginUndoAction(editor);

                if (viewModel.IsSqlNormalizedDisplay)
                {
                    ScintillaManager.ReplaceTextRange(editor, 0, currentText.Length, viewModel.RemoteRawText);
                }
                else
                {
                    ScintillaManager.ReplaceTextRange(
                        editor,
                        hunk.LocalRawStartIndex,
                        hunk.LocalRawEndIndex,
                        hunk.RemoteRawText);
                }
            }
            finally
            {
                ScintillaManager.EndUndoAction(editor);
            }

            editor.ContentString = ScintillaManager.GetScintillaText(editor);

            var updatedModelResult = BuildViewModel(editor, comparisonSession);
            if (updatedModelResult.Model == null)
            {
                return new ComparisonDiffActionResult
                {
                    Status = ComparisonDiffActionStatus.Error,
                    Message = updatedModelResult.Message,
                    MessageTitle = updatedModelResult.Title,
                    MessageIcon = updatedModelResult.Icon
                };
            }

            return new ComparisonDiffActionResult
            {
                Status = ComparisonDiffActionStatus.Applied,
                UpdatedModel = updatedModelResult.Model
            };
        }

        private (ComparisonDiffViewModel? Model, string? Message, string Title, MessageBoxIcon Icon) BuildViewModel(
            ScintillaEditor editor,
            ComparisonConnectionSession comparisonSession)
        {
            if (!EditorOpenTargetResolver.TryResolve(editor, out OpenTarget? openTarget, out string failureReason) || openTarget == null)
            {
                return (null,
                    $"Unable to resolve the current editor for comparison.\n\n{failureReason}",
                    "Comparison Error",
                    MessageBoxIcon.Warning);
            }

            string localRawText = ScintillaManager.GetScintillaText(editor) ?? string.Empty;
            string? remoteRawText = FetchComparisonText(openTarget, comparisonSession);

            if (remoteRawText == null)
            {
                return (null,
                    $"Unable to load comparison content for:\n{editor.Caption}",
                    "Comparison Not Available",
                    MessageBoxIcon.Warning);
            }

            var rebuilt = RebuildFromTexts(
                editor.AppDesignerProcess.DBName ?? "Current",
                comparisonSession.DatabaseName,
                openTarget.Type,
                editor.Caption,
                localRawText,
                remoteRawText,
                openTarget);

            return (rebuilt.UpdatedModel, rebuilt.Message, rebuilt.MessageTitle, rebuilt.MessageIcon);
        }

        private ComparisonDiffActionResult RebuildFromTexts(
            string localSourceName,
            string remoteSourceName,
            OpenTargetType openTargetType,
            string caption,
            string localRawText,
            string remoteRawText,
            OpenTarget? openTarget)
        {
            bool normalizeSqlForDisplay = openTargetType == OpenTargetType.SQL;
            string localDisplayText = normalizeSqlForDisplay ? ScintillaManager.NormalizeSqlForDiff(localRawText) : localRawText;
            string remoteDisplayText = normalizeSqlForDisplay ? ScintillaManager.NormalizeSqlForDiff(remoteRawText) : remoteRawText;

            string title = $"Diff: {localSourceName} vs {remoteSourceName} - {caption}";

            var diffBuilder = new SideBySideDiffBuilder(new Differ());
            var diff = diffBuilder.BuildDiffModel(localDisplayText, remoteDisplayText);

            var rows = new List<ComparisonDiffRow>();
            var hunks = BuildHunks(diff, rows, localDisplayText, remoteDisplayText, localRawText, remoteRawText, normalizeSqlForDisplay);

            return new ComparisonDiffActionResult
            {
                Status = ComparisonDiffActionStatus.Refreshed,
                UpdatedModel = new ComparisonDiffViewModel
                {
                    Title = title,
                    LocalSourceName = localSourceName,
                    RemoteSourceName = remoteSourceName,
                    LocalRawText = localRawText,
                    RemoteRawText = remoteRawText,
                    LocalDisplayText = localDisplayText,
                    RemoteDisplayText = remoteDisplayText,
                    OpenTarget = openTarget,
                    IsSqlNormalizedDisplay = normalizeSqlForDisplay,
                    HasDifferences = hunks.Count > 0,
                    Rows = rows,
                    Hunks = hunks,
                    OpenTargetType = openTargetType
                }
            };
        }

        private static List<ComparisonDiffHunk> BuildHunks(
            SideBySideDiffModel diff,
            List<ComparisonDiffRow> rows,
            string localDisplayText,
            string remoteDisplayText,
            string localRawText,
            string remoteRawText,
            bool normalizeSqlForDisplay)
        {
            var hunks = new List<ComparisonDiffHunk>();

            int localDisplayLineCursor = 0;
            int remoteDisplayLineCursor = 0;
            int? activeHunkId = null;
            int activeHunkStartRow = 0;
            int activeLocalStartLine = 0;
            int activeRemoteStartLine = 0;
            int activeLocalEndLine = 0;
            int activeRemoteEndLine = 0;
            bool sqlApplyButtonAssigned = false;

            for (int rowIndex = 0; rowIndex < diff.OldText.Lines.Count; rowIndex++)
            {
                var leftPiece = diff.OldText.Lines[rowIndex];
                var rightPiece = diff.NewText.Lines[rowIndex];

                bool changed = IsChanged(leftPiece.Type) || IsChanged(rightPiece.Type);

                int rowLocalStart = localDisplayLineCursor;
                int rowRemoteStart = remoteDisplayLineCursor;

                if (ConsumesLine(leftPiece.Type))
                {
                    localDisplayLineCursor++;
                }

                if (ConsumesLine(rightPiece.Type))
                {
                    remoteDisplayLineCursor++;
                }

                int rowLocalEnd = localDisplayLineCursor;
                int rowRemoteEnd = remoteDisplayLineCursor;

                if (changed && activeHunkId == null)
                {
                    activeHunkId = hunks.Count + 1;
                    activeHunkStartRow = rowIndex;
                    activeLocalStartLine = rowLocalStart;
                    activeRemoteStartLine = rowRemoteStart;
                }

                if (changed)
                {
                    activeLocalEndLine = rowLocalEnd;
                    activeRemoteEndLine = rowRemoteEnd;
                }

                bool closesHunk = activeHunkId != null && (!changed || rowIndex == diff.OldText.Lines.Count - 1);
                bool showButton = changed && activeHunkId != null && rowIndex == activeHunkStartRow;
                string actionText = "Pull";

                if (showButton && normalizeSqlForDisplay)
                {
                    if (sqlApplyButtonAssigned)
                    {
                        showButton = false;
                        actionText = string.Empty;
                    }
                    else
                    {
                        sqlApplyButtonAssigned = true;
                        actionText = "Pull All";
                    }
                }
                else if (!showButton)
                {
                    actionText = string.Empty;
                }

                rows.Add(new ComparisonDiffRow
                {
                    LeftText = leftPiece.Text ?? string.Empty,
                    RightText = rightPiece.Text ?? string.Empty,
                    LeftChangeType = leftPiece.Type,
                    RightChangeType = rightPiece.Type,
                    HunkId = changed ? activeHunkId : null,
                    ShowPullButton = showButton,
                    ActionText = actionText
                });

                if (closesHunk && activeHunkId != null)
                {
                    int closedEndRow = changed ? rowIndex : rowIndex - 1;
                    hunks.Add(CreateHunk(
                        activeHunkId.Value,
                        activeHunkStartRow,
                        closedEndRow,
                        activeLocalStartLine,
                        activeLocalEndLine,
                        activeRemoteStartLine,
                        activeRemoteEndLine,
                        localDisplayText,
                        remoteDisplayText,
                        localRawText,
                        remoteRawText,
                        normalizeSqlForDisplay));

                    activeHunkId = null;
                }
            }

            return hunks;
        }

        private static ComparisonDiffHunk CreateHunk(
            int hunkId,
            int startRowIndex,
            int endRowIndex,
            int localStartLine,
            int localEndLine,
            int remoteStartLine,
            int remoteEndLine,
            string localDisplayText,
            string remoteDisplayText,
            string localRawText,
            string remoteRawText,
            bool normalizeSqlForDisplay)
        {
            var localDisplayIndex = BuildLineIndex(localDisplayText);
            var remoteDisplayIndex = BuildLineIndex(remoteDisplayText);
            var localRawIndex = BuildLineIndex(localRawText);
            var remoteRawIndex = BuildLineIndex(remoteRawText);

            int localDisplayStartIndex = GetCharIndexForLine(localDisplayIndex, localStartLine);
            int localDisplayEndIndex = GetCharIndexForLine(localDisplayIndex, localEndLine);
            int remoteDisplayStartIndex = GetCharIndexForLine(remoteDisplayIndex, remoteStartLine);
            int remoteDisplayEndIndex = GetCharIndexForLine(remoteDisplayIndex, remoteEndLine);

            string remoteDisplaySegment = remoteDisplayText.Substring(
                remoteDisplayStartIndex,
                Math.Max(0, remoteDisplayEndIndex - remoteDisplayStartIndex));

            int localRawStartIndex = normalizeSqlForDisplay ? 0 : GetCharIndexForLine(localRawIndex, localStartLine);
            int localRawEndIndex = normalizeSqlForDisplay ? 0 : GetCharIndexForLine(localRawIndex, localEndLine);
            int remoteRawStartIndex = normalizeSqlForDisplay ? 0 : GetCharIndexForLine(remoteRawIndex, remoteStartLine);
            int remoteRawEndIndex = normalizeSqlForDisplay ? 0 : GetCharIndexForLine(remoteRawIndex, remoteEndLine);

            string remoteRawSegment = normalizeSqlForDisplay
                ? remoteRawText
                : remoteRawText.Substring(remoteRawStartIndex, Math.Max(0, remoteRawEndIndex - remoteRawStartIndex));

            return new ComparisonDiffHunk
            {
                Id = hunkId,
                StartRowIndex = startRowIndex,
                EndRowIndex = endRowIndex,
                LocalStartLine = localStartLine,
                LocalEndLine = localEndLine,
                RemoteStartLine = remoteStartLine,
                RemoteEndLine = remoteEndLine,
                LocalDisplayStartIndex = localDisplayStartIndex,
                LocalDisplayEndIndex = localDisplayEndIndex,
                RemoteDisplayStartIndex = remoteDisplayStartIndex,
                RemoteDisplayEndIndex = remoteDisplayEndIndex,
                LocalRawStartIndex = localRawStartIndex,
                LocalRawEndIndex = localRawEndIndex,
                RemoteDisplayText = remoteDisplaySegment,
                RemoteRawText = remoteRawSegment,
                Summary = $"Lines {localStartLine + 1}-{Math.Max(localStartLine + 1, localEndLine)}",
                AppliesWholeDocument = normalizeSqlForDisplay
            };
        }

        private static bool IsChanged(ChangeType changeType)
        {
            return changeType != ChangeType.Unchanged && changeType != ChangeType.Imaginary;
        }

        private static bool ConsumesLine(ChangeType changeType)
        {
            return changeType != ChangeType.Imaginary;
        }

        private static TextLineIndex BuildLineIndex(string text)
        {
            var starts = new List<int> { 0 };
            for (int i = 0; i < text.Length; i++)
            {
                if (text[i] == '\n')
                {
                    starts.Add(i + 1);
                }
            }

            return new TextLineIndex
            {
                LineStarts = starts.ToArray(),
                LineCount = starts.Count,
                TextLength = text.Length
            };
        }

        private static int GetCharIndexForLine(TextLineIndex lineIndex, int lineNumber)
        {
            if (lineNumber <= 0)
            {
                return 0;
            }

            if (lineNumber >= lineIndex.LineCount)
            {
                return lineIndex.TextLength;
            }

            return lineIndex.LineStarts[lineNumber];
        }

        private static string? FetchComparisonText(OpenTarget openTarget, ComparisonConnectionSession comparisonSession)
        {
            try
            {
                return openTarget.Type switch
                {
                    OpenTargetType.SQL => comparisonSession.DataManager.GetSqlDefinition(openTarget.Name),
                    _ => comparisonSession.DataManager.GetPeopleCodeProgram(openTarget)
                };
            }
            catch (Exception ex)
            {
                AppRefiner.Debug.Log($"EditorComparisonService.FetchComparisonText: Error fetching comparison text for {openTarget.Name}: {ex.Message}");
                return null;
            }
        }

        private static void ShowMessage(string message, string title, MessageBoxIcon icon, IntPtr ownerHandle)
        {
            new MessageBoxDialog(message, title, MessageBoxButtons.OK, ownerHandle)
                .ShowDialog(new WindowWrapper(ownerHandle));
        }
    }
}
