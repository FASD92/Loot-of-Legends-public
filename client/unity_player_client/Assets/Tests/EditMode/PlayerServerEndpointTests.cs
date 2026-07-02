using LootOfLegends.PlayerClient;
using NUnit.Framework;

namespace LootOfLegends.PlayerClient.Tests
{
    public sealed class PlayerServerEndpointTests
    {
        [Test]
        public void DefaultEndpointUsesLoopbackAndServerPort()
        {
            PlayerServerEndpoint endpoint = PlayerServerEndpoint.Default;

            Assert.True(endpoint.IsValid);
            Assert.AreEqual("127.0.0.1", endpoint.Host);
            Assert.AreEqual(40000, endpoint.TcpPort);
            Assert.AreEqual(40000, endpoint.RudpPort);
            Assert.AreEqual("127.0.0.1 tcp=40000 udp=40000", endpoint.DisplayName);
        }

        [Test]
        public void TryCreateRejectsBlankHostOrInvalidPorts()
        {
            Assert.False(PlayerServerEndpoint.TryCreate(" ", 40000, 40000, out _));
            Assert.False(PlayerServerEndpoint.TryCreate("127.0.0.1", 0, 40000, out _));
            Assert.False(PlayerServerEndpoint.TryCreate("127.0.0.1", 40000, 70000, out _));
        }
    }
}
