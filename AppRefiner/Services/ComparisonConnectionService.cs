using AppRefiner.Database;
using AppRefiner.Dialogs;

namespace AppRefiner.Services
{
    /// <summary>
    /// Creates temporary comparison database sessions by reusing the standard DB connection dialog.
    /// </summary>
    public sealed class ComparisonConnectionService
    {
        public ComparisonConnectionSession? PromptForSession(IntPtr ownerHandle, string? defaultDbName = null)
        {
            var handleWrapper = new WindowWrapper(ownerHandle);
            using var dialog = new DBConnectDialog(
                ownerHandle,
                defaultDbName,
                headerTitle: "Comparison Database",
                windowTitle: "Connect Comparison Database",
                connectButtonText: "Connect");

            dialog.StartPosition = FormStartPosition.CenterParent;

            if (dialog.ShowDialog(handleWrapper) != DialogResult.OK || dialog.DataManager == null)
            {
                return null;
            }

            var databaseName = dialog.SelectedDatabaseName;
            if (string.IsNullOrWhiteSpace(databaseName))
            {
                databaseName = defaultDbName ?? "comparison";
            }

            return new ComparisonConnectionSession(databaseName, dialog.DataManager);
        }
    }
}
