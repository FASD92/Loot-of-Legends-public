#if UNITY_EDITOR
using System;
using System.IO;
using UnityEditor;
using UnityEditor.Build;
using UnityEditor.Build.Reporting;
using UnityEngine;

namespace LootOfLegends.Editor
{
    internal sealed class MacFinderIconFileCleaner : IPreprocessBuildWithReport
    {
        private const string FinderIconFileName = "Icon\r";
        private const string FinderIconMetaFileName = "Icon\r.meta";

        public int callbackOrder => int.MinValue;

        [MenuItem("Tools/Loot of Legends/Clean macOS Finder Icon Files")]
        public static void CleanFromMenu()
        {
            int deletedCount = DeleteFinderIconFiles();
            Debug.Log($"Removed {deletedCount} macOS Finder Icon carriage-return file(s).");
        }

        public void OnPreprocessBuild(BuildReport report)
        {
            int deletedCount = DeleteFinderIconFiles();
            if (deletedCount > 0)
            {
                Debug.Log($"Removed {deletedCount} macOS Finder Icon carriage-return file(s) before build.");
            }
        }

        private static int DeleteFinderIconFiles()
        {
            string projectRoot = Path.GetFullPath(Path.Combine(Application.dataPath, ".."));
            int deletedCount = DeleteFinderIconFilesUnder(projectRoot);

            if (deletedCount > 0)
            {
                AssetDatabase.Refresh(ImportAssetOptions.ForceUpdate);
            }

            return deletedCount;
        }

        private static int DeleteFinderIconFilesUnder(string root)
        {
            if (!Directory.Exists(root))
            {
                return 0;
            }

            int deletedCount = 0;
            string[] files;
            try
            {
                files = Directory.GetFiles(root);
            }
            catch (Exception ex) when (ex is IOException || ex is UnauthorizedAccessException)
            {
                Debug.LogWarning($"Skipped Finder Icon cleanup under '{root}': {ex.Message}");
                return 0;
            }

            foreach (string file in files)
            {
                string fileName = Path.GetFileName(file);
                if (fileName != FinderIconFileName && fileName != FinderIconMetaFileName)
                {
                    continue;
                }

                if (DeleteFile(file))
                {
                    deletedCount++;
                }
            }

            string[] directories;
            try
            {
                directories = Directory.GetDirectories(root);
            }
            catch (Exception ex) when (ex is IOException || ex is UnauthorizedAccessException)
            {
                Debug.LogWarning($"Skipped Finder Icon cleanup directories under '{root}': {ex.Message}");
                return deletedCount;
            }

            foreach (string directory in directories)
            {
                deletedCount += DeleteFinderIconFilesUnder(directory);
            }

            return deletedCount;
        }

        private static bool DeleteFile(string path)
        {
            string fullPath = Path.GetFullPath(path);
            try
            {
                File.Delete(fullPath);
                return true;
            }
            catch (Exception ex) when (ex is IOException || ex is UnauthorizedAccessException)
            {
                Debug.LogWarning($"Failed to delete Finder Icon file '{fullPath}': {ex.Message}");
                return false;
            }
        }
    }
}
#endif
