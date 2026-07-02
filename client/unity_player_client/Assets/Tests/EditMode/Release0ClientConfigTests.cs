using LootOfLegends.PlayerClient;
using NUnit.Framework;
using System;
using System.IO;

namespace LootOfLegends.PlayerClient.Tests
{
    public sealed class Release0ClientConfigTests
    {
        [Test]
        public void LoadMetaBaseUrlUsesStreamingAssetsConfig()
        {
            string tempDirectory = CreateTempDirectory();
            try
            {
                File.WriteAllText(
                    Path.Combine(tempDirectory, Release0ClientConfig.FileName),
                    "{ \"metaBaseUrl\": \" https://meta.release0.example.com/ \" }");

                string metaBaseUrl = Release0ClientConfig.LoadMetaBaseUrl(
                    tempDirectory,
                    Release0ClientConfig.DefaultMetaBaseUrl);

                Assert.AreEqual("https://meta.release0.example.com", metaBaseUrl);
            }
            finally
            {
                Directory.Delete(tempDirectory, true);
            }
        }

        [Test]
        public void LoadMetaBaseUrlFallsBackWhenConfigIsMissingOrBlank()
        {
            string tempDirectory = CreateTempDirectory();
            try
            {
                Assert.AreEqual(
                    "http://127.0.0.1:18080",
                    Release0ClientConfig.LoadMetaBaseUrl(
                        tempDirectory,
                        "http://127.0.0.1:18080/"));

                File.WriteAllText(
                    Path.Combine(tempDirectory, Release0ClientConfig.FileName),
                    "{ \"metaBaseUrl\": \"   \" }");

                Assert.AreEqual(
                    "http://127.0.0.1:18080",
                    Release0ClientConfig.LoadMetaBaseUrl(
                        tempDirectory,
                        "http://127.0.0.1:18080/"));
            }
            finally
            {
                Directory.Delete(tempDirectory, true);
            }
        }

        private static string CreateTempDirectory()
        {
            string tempDirectory = Path.Combine(
                Path.GetTempPath(),
                "release0-client-config-" + Guid.NewGuid().ToString("N"));
            Directory.CreateDirectory(tempDirectory);
            return tempDirectory;
        }
    }
}
