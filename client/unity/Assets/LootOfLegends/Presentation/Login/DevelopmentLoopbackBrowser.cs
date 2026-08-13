using System;
using System.Net.Http;
using System.Threading.Tasks;
using LootOfLegends.Session;
using UnityEngine;

namespace LootOfLegends.Presentation.Login
{
#if DEVELOPMENT_BUILD || UNITY_EDITOR
    public sealed class DevelopmentLoopbackBrowser : ISystemBrowser
    {
        private const string FixtureHost = "fixture.invalid";

        public void Open(Uri uri)
        {
            if (uri == null || uri.Scheme != Uri.UriSchemeHttps ||
                uri.Host != FixtureHost)
            {
                throw new InvalidOperationException(
                    "Development auth browser accepts only the local fixture handoff");
            }
            _ = CompleteAsync(uri);
        }

        private static async Task CompleteAsync(Uri authorizationUri)
        {
            try
            {
                string callback = ReadQuery(authorizationUri, "callback");
                string state = ReadQuery(authorizationUri, "state");
                string handoff = ReadQuery(authorizationUri, "handoff");
                var redirect = new Uri(
                    callback + "?code=" + Uri.EscapeDataString(handoff) +
                    "&state=" + Uri.EscapeDataString(state));
                using (var http = new HttpClient())
                using (HttpResponseMessage response = await http.GetAsync(redirect))
                {
                    response.EnsureSuccessStatusCode();
                }
            }
            catch (Exception)
            {
                Debug.LogWarning("Development browser handoff failed safely.");
            }
        }

        private static string ReadQuery(Uri uri, string name)
        {
            string query = uri.Query.TrimStart('?');
            foreach (string pair in query.Split('&'))
            {
                int separator = pair.IndexOf('=');
                if (separator <= 0)
                {
                    continue;
                }
                string key = Uri.UnescapeDataString(pair.Substring(0, separator));
                if (key == name)
                {
                    return Uri.UnescapeDataString(pair.Substring(separator + 1));
                }
            }
            throw new InvalidOperationException(
                "Development auth handoff query is incomplete");
        }
    }
#endif
}
