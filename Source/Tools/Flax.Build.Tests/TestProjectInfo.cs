// Copyright (c) Wojciech Figat. All rights reserved.

using System;
using System.Diagnostics;
using System.IO;
using NUnit.Framework;

namespace Flax.Build.Tests
{
    [TestFixture]
    public class TestProjectInfo
    {
        [Test]
        public void TestVersionControlInfoInGitWorktree()
        {
            var root = Path.Combine(Path.GetTempPath(), "FlaxBuildTests", Guid.NewGuid().ToString("N"));
            var worktree = Path.Combine(root, "worktree");
            Directory.CreateDirectory(root);
            try
            {
                RunGit(root, "init -q --initial-branch=main");
                File.WriteAllText(Path.Combine(root, "Test.flaxproj"), "{\"Name\":\"Test\",\"Version\":\"1.0\"}");
                RunGit(root, "add Test.flaxproj");
                RunGit(root, "-c user.name=FlaxTests -c user.email=tests@flaxengine.com commit -q -m \"worktree fixture\"");
                RunGit(root, $"worktree add -q --detach \"{worktree}\" HEAD");

                Assert.IsTrue(File.Exists(Path.Combine(worktree, ".git")));
                var project = ProjectInfo.Load(Path.Combine(worktree, "Test.flaxproj"));
                Assert.IsNotEmpty(project.VersionControlCommit);
                Assert.AreEqual("worktree fixture", project.VersionControlCommitName);
            }
            finally
            {
                if (Directory.Exists(worktree))
                    RunGit(root, $"worktree remove --force \"{worktree}\"");
                foreach (var path in Directory.EnumerateFiles(root, "*", SearchOption.AllDirectories))
                    File.SetAttributes(path, FileAttributes.Normal);
                Directory.Delete(root, true);
            }
        }

        private static void RunGit(string workingDirectory, string arguments)
        {
            using var process = Process.Start(new ProcessStartInfo("git", arguments)
            {
                WorkingDirectory = workingDirectory,
                UseShellExecute = false,
                RedirectStandardError = true,
                RedirectStandardOutput = true,
                CreateNoWindow = true,
            });
            Assert.IsNotNull(process);
            process.WaitForExit();
            Assert.AreEqual(0, process.ExitCode, process.StandardError.ReadToEnd());
        }
    }
}
