using System;
using LootOfLegends.Session;
using UnityEngine;

namespace LootOfLegends.Presentation.Login
{
    public sealed class UnitySystemBrowser : ISystemBrowser
    {
        public void Open(Uri uri)
        {
            if (uri == null || !uri.IsAbsoluteUri || uri.Scheme != Uri.UriSchemeHttps)
            {
                throw new ArgumentException("System browser URL must be absolute HTTPS", nameof(uri));
            }
            Application.OpenURL(uri.AbsoluteUri);
        }
    }
}
