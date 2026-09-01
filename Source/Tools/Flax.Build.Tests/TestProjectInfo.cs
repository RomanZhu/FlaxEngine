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
        private const string CurrentProject = "{\"Name\":\"Test\",\"Version\":\"1.0\",\"AssetSystemVersion\":2,\"ProjectSettingsIndexGuid\":\"36f15f0c4b354af88ba2f72f6cb82e22\",\"Company\":\"\",\"Copyright\":\"\",\"GameTarget\":\"Game\",\"EditorTarget\":\"Editor\",\"References\":[],\"MinEngineVersion\":\"1.0\"}";

        [Test]
        public void TestVersionControlInfoInGitWorktree()
        {
            var root = Path.Combine(Path.GetTempPath(), "FlaxBuildTests", Guid.NewGuid().ToString("N"));
            var worktree = Path.Combine(root, "worktree");
            Directory.CreateDirectory(root);
            try
            {
                RunGit(root, "init -q --initial-branch=main");
                File.WriteAllText(Path.Combine(root, "Test.flaxproj"), CurrentProject);
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

        [TestCase("{\"Name\":\"Test\",\"Version\":\"1.0\"}")]
        [TestCase("{\"Name\":\"Test\",\"Version\":{\"Major\":1},\"AssetSystemVersion\":2}")]
        [TestCase("{\"Name\":\"Test\",\"Version\":\"1.0\",\"AssetSystemVersion\":1}")]
        [TestCase("{\"Name\":\"Test\",\"Version\":\"1.0\",\"AssetSystemVersion\":3}")]
        [TestCase("{\"Name\":\"Test\",\"Version\":\"1.0\",\"AssetSystemVersion\":2,\"AssetSystemVersion\":1}")]
        [TestCase("{\"Name\":\"Test\",\"Version\":\"1.0\",\"AssetSystemVersion\":2,\"ProjectSettingsIndexGuid\":\"36f15f0c4b354af88ba2f72f6cb82e22\",\"Company\":\"\",\"Copyright\":\"\",\"GameTarget\":\"Game\",\"EditorTarget\":\"Editor\",\"References\":[],\"MinEngineVersion\":\"1.0\",\"DefaultScene\":\"36f15f0c4b354af88ba2f72f6cb82e22\"}")]
        [TestCase("{\"Name\":\"Test\",\"Version\":\"1.0\",\"AssetSystemVersion\":2,\"ProjectSettingsIndexGuid\":\"36f15f0c4b354af88ba2f72f6cb82e22\",\"Company\":\"\",\"Copyright\":\"\",\"GameTarget\":\"Game\",\"EditorTarget\":\"Editor\",\"References\":[],\"MinEngineVersion\":\"1.0\",\"DefaultSceneSpawn\":{\"Position\":[0,0,0]}}")]
        public void TestRejectsUnsupportedProjectFormatWithoutMutation(string contents)
        {
            var root = Path.Combine(Path.GetTempPath(), "FlaxBuildTests", Guid.NewGuid().ToString("N"));
            var path = Path.Combine(root, "Test.flaxproj");
            Directory.CreateDirectory(root);
            try
            {
                File.WriteAllText(path, contents);
                var before = File.ReadAllBytes(path);
                var exception = Assert.Throws<InvalidDataException>(() => ProjectInfo.Load(path));
                StringAssert.Contains("offline migrator", exception.Message);
                CollectionAssert.AreEqual(before, File.ReadAllBytes(path));
            }
            finally
            {
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
