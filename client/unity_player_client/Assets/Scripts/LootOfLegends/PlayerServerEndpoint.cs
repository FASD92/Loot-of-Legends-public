namespace LootOfLegends.PlayerClient
{
    public readonly struct PlayerServerEndpoint
    {
        public const string DefaultHost = "127.0.0.1";
        public const ushort DefaultTcpPort = 40000;
        public const ushort DefaultRudpPort = 40000;

        public readonly string Host;
        public readonly ushort TcpPort;
        public readonly ushort RudpPort;

        private PlayerServerEndpoint(string host, ushort tcpPort, ushort rudpPort)
        {
            Host = host;
            TcpPort = tcpPort;
            RudpPort = rudpPort;
        }

        public static PlayerServerEndpoint Default =>
            new PlayerServerEndpoint(DefaultHost, DefaultTcpPort, DefaultRudpPort);

        public bool IsValid => !string.IsNullOrWhiteSpace(Host) && TcpPort != 0 && RudpPort != 0;

        public string DisplayName => Host + " tcp=" + TcpPort + " udp=" + RudpPort;

        public static bool TryCreate(
            string host,
            int tcpPort,
            int rudpPort,
            out PlayerServerEndpoint endpoint)
        {
            endpoint = default;
            if (string.IsNullOrWhiteSpace(host) ||
                tcpPort <= 0 ||
                tcpPort > ushort.MaxValue ||
                rudpPort <= 0 ||
                rudpPort > ushort.MaxValue)
            {
                return false;
            }

            endpoint = new PlayerServerEndpoint(
                host.Trim(),
                (ushort)tcpPort,
                (ushort)rudpPort);
            return true;
        }
    }
}
