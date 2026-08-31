// Copyright (c) Wojciech Figat. All rights reserved.

using System.Text;
using FlaxEngine;
using FlaxEngine.GUI;

namespace FlaxEditor.Windows
{
    /// <summary>
    /// Editor window for canonical asset database records, diagnostics, and Library repair commands.
    /// </summary>
    public sealed class AssetPipelineWindow : EditorWindow
    {
        private readonly Label _summary;
        private readonly Label _records;
        private readonly Label _diagnostics;

        /// <summary>
        /// Initializes a new instance of the <see cref="AssetPipelineWindow"/> class.
        /// </summary>
        public AssetPipelineWindow(Editor editor)
        : base(editor, true, ScrollBars.Vertical)
        {
            Title = "Asset Import Activity";

            var toolbar = new HorizontalPanel
            {
                Parent = this,
                Height = 32,
                Width = 640,
                Offsets = new Margin(8, 8, 8, 8),
            };
            var refresh = new Button
            {
                Parent = toolbar,
                Text = "Refresh",
                Width = 90,
                TooltipText = "Reconcile metadata and import changed canonical sources.",
            };
            refresh.Clicked += () =>
            {
                AssetDatabaseFacade.Refresh(ImportAssetOptions.Default);
                RefreshView();
            };
            var reimport = new Button
            {
                Parent = toolbar,
                Text = "Reimport All",
                Width = 110,
                TooltipText = "Force a fresh build for every supported source.",
            };
            reimport.Clicked += () =>
            {
                AssetDatabaseFacade.Refresh(ImportAssetOptions.ForceUpdate);
                RefreshView();
            };
            var clean = new Button
            {
                Parent = toolbar,
                Text = "Clean Library",
                Width = 120,
                TooltipText = "Delete generated Library artifacts. Canonical sources are not modified.",
            };
            clean.Clicked += () =>
            {
                AssetDatabaseFacade.CleanLibrary();
                RefreshView();
            };
            var inventory = new Button
            {
                Parent = toolbar,
                Text = "Inventory",
                Width = 100,
                TooltipText = "Build a read-only mixed-mode migration inventory.",
            };
            inventory.Clicked += () =>
            {
                AssetDatabaseFacade.Scan(false);
                RefreshView();
            };

            _summary = new Label
            {
                Parent = this,
                AutoWidth = false,
                AutoHeight = true,
                HorizontalAlignment = TextAlignment.Near,
                Wrapping = TextWrapping.WrapWords,
                Offsets = new Margin(8, 8, 48, 8),
            };
            _records = new Label
            {
                Parent = this,
                AutoWidth = false,
                AutoHeight = true,
                HorizontalAlignment = TextAlignment.Near,
                VerticalAlignment = TextAlignment.Near,
                Wrapping = TextWrapping.WrapWords,
                Offsets = new Margin(8, 8, 88, 8),
            };
            _diagnostics = new Label
            {
                Parent = this,
                AutoWidth = false,
                AutoHeight = true,
                HorizontalAlignment = TextAlignment.Near,
                VerticalAlignment = TextAlignment.Near,
                Wrapping = TextWrapping.WrapWords,
                Offsets = new Margin(8, 8, 280, 8),
            };
            RefreshView();
        }

        /// <inheritdoc />
        protected override void OnShow()
        {
            RefreshView();
            base.OnShow();
        }

        private void RefreshView()
        {
            var records = AssetDatabaseFacade.QueryRecords(default);
            var diagnostics = AssetDatabaseFacade.GetDiagnostics();
            _summary.Text = "Revision " + AssetDatabaseFacade.Revision + " · " + records.Length + " records · " + diagnostics.Length + " diagnostics";

            var recordsText = new StringBuilder();
            recordsText.AppendLine("Records");
            var limit = records.Length < 200 ? records.Length : 200;
            for (int i = 0; i < limit; i++)
            {
                var record = records[i];
                recordsText.Append(record.Status).Append("  ")
                    .Append(record.SourceAssetID.ToString("N")).Append(':').Append(record.LocalId).Append("  ")
                    .Append(record.SourceKind).Append("  ")
                    .Append(record.ProcessorID).Append("  ")
                    .AppendLine(record.CanonicalPath);
            }
            if (records.Length > limit)
                recordsText.Append("… ").Append(records.Length - limit).AppendLine(" more");
            _records.Text = recordsText.ToString();

            var diagnosticsText = new StringBuilder();
            diagnosticsText.AppendLine("Diagnostics");
            if (diagnostics.Length == 0)
                diagnosticsText.AppendLine("None.");
            for (int i = 0; i < diagnostics.Length && i < 50; i++)
            {
                var diagnostic = diagnostics[i];
                diagnosticsText.Append(diagnostic.Severity).Append("  ").Append(diagnostic.Code).Append(": ").AppendLine(diagnostic.Message);
            }
            _diagnostics.Text = diagnosticsText.ToString();
        }
    }
}
