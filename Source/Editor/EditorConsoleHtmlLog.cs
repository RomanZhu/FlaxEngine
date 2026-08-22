// Copyright (c) Wojciech Figat. All rights reserved.

using System;
using System.IO;
using System.Net;
using System.Text;
using FlaxEngine;

namespace FlaxEditor
{
    internal sealed class EditorConsoleHtmlLog : IDisposable
    {
        private readonly object _locker = new object();
        private readonly StreamWriter _writer;
        private bool _disposed;

        public string FolderPath { get; }
        public string FilePath { get; }

        public EditorConsoleHtmlLog()
        {
            FolderPath = Path.Combine(Globals.ProjectFolder, "Logs");
            Directory.CreateDirectory(FolderPath);
            FilePath = Path.Combine(FolderPath, $"EditorConsole-{DateTime.Now:yyyyMMdd-HHmmss-fff}-P{Environment.ProcessId}-{Guid.NewGuid():N}.html");
            var stream = new FileStream(FilePath, FileMode.CreateNew, FileAccess.Write, FileShare.ReadWrite);
            _writer = new StreamWriter(stream, Encoding.UTF8) { AutoFlush = true };
            WriteHeader();
        }

        public void Append(DateTime timestamp, ulong threadId, string kind, string message, string stackTrace)
        {
            kind = (kind ?? "info").ToLowerInvariant();
            message = WebUtility.HtmlEncode(message ?? string.Empty);
            bool includeStackTrace = kind == "warning" || kind == "error" || kind == "fatal";
            stackTrace = includeStackTrace
                ? WebUtility.HtmlEncode(stackTrace ?? string.Empty)
                : string.Empty;
            lock (_locker)
            {
                if (_disposed)
                    return;
                _writer.Write($"<article class='entry {kind}' data-kind='{kind}'>");
                _writer.Write($"<time>{timestamp:HH:mm:ss.fff}</time><span class='thread'>T{threadId}</span>");
                _writer.Write($"<span class='level'>{WebUtility.HtmlEncode(kind.ToUpperInvariant())}</span><pre class='message'>{message}</pre>");
                if (!string.IsNullOrWhiteSpace(stackTrace))
                    _writer.Write($"<details><summary>Stack trace</summary><pre>{stackTrace}</pre></details>");
                _writer.WriteLine("</article>");
            }
        }

        public void Open()
        {
            Platform.OpenUrl(new Uri(FilePath).AbsoluteUri);
        }

        public void OpenFolder()
        {
            FileSystem.ShowFileExplorer(FolderPath);
        }

        public void Dispose()
        {
            lock (_locker)
            {
                if (_disposed)
                    return;
                _disposed = true;
                _writer.WriteLine("</main></body></html>");
                _writer.Dispose();
            }
        }

        private void WriteHeader()
        {
            string title = $"Flax Editor Console — {DateTime.Now:yyyy-MM-dd HH:mm:ss}";
            _writer.WriteLine("<!doctype html><html lang='en'><head><meta charset='utf-8'>");
            _writer.WriteLine($"<title>{WebUtility.HtmlEncode(title)}</title>");
            _writer.WriteLine("<style>body{margin:0;background:#11151a;color:#dbe2ea;font:14px/1.4 Consolas,monospace}header{position:sticky;top:0;padding:12px 16px;background:#1b222b;border-bottom:1px solid #394553}button{margin:0 5px 0 0;padding:5px 9px;background:#2a3542;color:#e9eef4;border:1px solid #4a5969;border-radius:3px}.entry{display:grid;grid-template-columns:105px 70px 78px 1fr;gap:8px;padding:5px 16px;border-bottom:1px solid #222b35}.message,.entry pre{white-space:pre-wrap;margin:0}.thread,time{color:#8995a3}.info .level{color:#c7d0da}.warning .level{color:#f2c14e}.error .level,.fatal .level{color:#ff6577}.command .level{color:#68b9ff}.result .level{color:#75dda2}details{grid-column:4;padding-top:4px;color:#aab4bf}.hidden{display:none}</style>");
            _writer.WriteLine("<script>function filter(k){document.querySelectorAll('.entry').forEach(e=>e.classList.toggle('hidden',k!=='all'&&e.dataset.kind!==k))}</script></head><body>");
            _writer.WriteLine($"<header><strong>{WebUtility.HtmlEncode(title)}</strong><div><button onclick=\"filter('all')\">All</button><button onclick=\"filter('info')\">Info</button><button onclick=\"filter('warning')\">Warnings</button><button onclick=\"filter('error')\">Errors</button><button onclick=\"filter('fatal')\">Fatal</button><button onclick=\"filter('command')\">Commands</button><button onclick=\"filter('result')\">Results</button></div></header><main id='log'>");
        }
    }
}
