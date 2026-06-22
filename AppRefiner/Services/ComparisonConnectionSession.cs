using AppRefiner.Database;

namespace AppRefiner.Services
{
    /// <summary>
    /// Represents a temporary comparison database connection used by the diff feature.
    /// </summary>
    public sealed class ComparisonConnectionSession : IDisposable
    {
        public ComparisonConnectionSession(string databaseName, IDataManager dataManager)
        {
            DatabaseName = databaseName;
            DataManager = dataManager;
        }

        public string DatabaseName { get; }

        public IDataManager DataManager { get; }

        public void Dispose()
        {
            try
            {
                DataManager.Disconnect();
            }
            catch (Exception ex)
            {
                AppRefiner.Debug.Log($"ComparisonConnectionSession.Dispose: Error disconnecting {DatabaseName}: {ex.Message}");
            }
        }
    }
}
