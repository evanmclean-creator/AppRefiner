using AppRefiner.Database;
using AppRefiner.Database.Models;
using System.Text;

namespace AppRefiner
{
    /// <summary>
    /// Resolves the currently open PeopleCode item or SQL definition for a Scintilla editor into an OpenTarget.
    /// </summary>
    public static class EditorOpenTargetResolver
    {
        public static bool TryResolve(ScintillaEditor? editor, out OpenTarget? openTarget, out string failureReason)
        {
            openTarget = null;

            if (editor == null)
            {
                failureReason = "No active editor.";
                return false;
            }

            if (string.IsNullOrWhiteSpace(editor.Caption))
            {
                failureReason = "The active editor does not have a caption yet.";
                return false;
            }

            if (editor.Type != EditorType.PeopleCode && editor.Type != EditorType.SQL)
            {
                failureReason = $"The active editor is {editor.Type}, not a PeopleCode or SQL editor.";
                return false;
            }

            openTarget = OpenTargetBuilder.CreateFromCaption(editor.Caption);
            if (openTarget == null)
            {
                failureReason = $"Could not parse the editor caption '{editor.Caption}'.";
                return false;
            }

            failureReason = string.Empty;
            return true;
        }

        public static string BuildDiagnosticText(ScintillaEditor? editor, OpenTarget? openTarget, string? failureReason = null)
        {
            var builder = new StringBuilder();

            builder.AppendLine($"Caption: {editor?.Caption ?? "(null)"}");
            builder.AppendLine($"Editor Type: {editor?.Type.ToString() ?? "(null)"}");

            if (openTarget == null)
            {
                builder.AppendLine($"Resolved: No");

                if (!string.IsNullOrWhiteSpace(failureReason))
                {
                    builder.AppendLine($"Reason: {failureReason}");
                }

                return builder.ToString().TrimEnd();
            }

            builder.AppendLine("Resolved: Yes");
            builder.AppendLine($"OpenTarget Type: {openTarget.Type}");
            builder.AppendLine($"Name: {openTarget.Name}");
            builder.AppendLine($"Description: {openTarget.Description}");
            builder.AppendLine($"Path: {openTarget.Path}");
            builder.AppendLine($"Qualified Name: {openTarget.ToQualifiedName()}");
            builder.AppendLine("Object Path:");

            for (int i = 0; i < openTarget.ObjectIDs.Length; i++)
            {
                if (openTarget.ObjectIDs[i] == PSCLASSID.NONE || string.IsNullOrWhiteSpace(openTarget.ObjectValues[i]))
                {
                    continue;
                }

                builder.AppendLine($"  {i + 1}. {openTarget.ObjectIDs[i]} = {openTarget.ObjectValues[i]}");
            }

            return builder.ToString().TrimEnd();
        }
    }
}
