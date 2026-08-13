"""Normal Meta credential -> TCP auth -> RUDP bind player boundary."""

from __future__ import annotations

from dataclasses import dataclass
from typing import Any

from ..protocol.meta import MetaHttpClient
from ..protocol.rudp import RudpMessage, RudpProtocolPeer
from ..protocol.tcp import TcpMessage, TcpProtocolClient


class BoundaryAuthenticationRejected(RuntimeError):
    def __init__(self, reason: str) -> None:
        super().__init__(f"game authentication rejected: {reason}")
        self.reason = reason


@dataclass(frozen=True)
class SessionIdentity:
    session_id: int
    session_generation: int
    nickname: str


class BoundaryGameClient:
    def __init__(
        self,
        *,
        meta: MetaHttpClient,
        meta_session: str,
        tcp: TcpProtocolClient,
        rudp: RudpProtocolPeer,
        identity: SessionIdentity,
        welcome: TcpMessage,
        bind_accepted: RudpMessage,
    ) -> None:
        self._meta = meta
        self._meta_session = meta_session
        self.tcp = tcp
        self.rudp = rudp
        self.identity = identity
        self.welcome = welcome
        self.bind_accepted = bind_accepted

    @classmethod
    async def connect(
        cls,
        *,
        meta: MetaHttpClient,
        meta_session: str,
        game_host: str,
        tcp_port: int,
        rudp_port: int,
        tcp_ssl: Any = None,
        timeout_seconds: float = 5.0,
    ) -> BoundaryGameClient:
        credential = await meta.issue_game_credential(meta_session)
        tcp = await TcpProtocolClient.connect(
            game_host, tcp_port, ssl=tcp_ssl, timeout_seconds=timeout_seconds
        )
        rudp: RudpProtocolPeer | None = None
        try:
            response = await tcp.request(
                "AuthenticateGameSession",
                {"credential": credential["credential"]},
                expected={"Welcome", "AuthenticationRejected"},
                timeout_seconds=timeout_seconds,
            )
            if response.name == "AuthenticationRejected":
                raise BoundaryAuthenticationRejected(response.fields["reason"])
            identity = SessionIdentity(
                response.fields["sessionId"],
                response.fields["sessionGeneration"],
                response.fields["nickname"],
            )
            capability = await tcp.request(
                "RequestRudpBindCapability",
                {},
                expected="RudpBindCapability",
                timeout_seconds=timeout_seconds,
            )
            rudp = await RudpProtocolPeer.open(
                game_host,
                rudp_port,
                session_id=identity.session_id,
                session_generation=identity.session_generation,
            )
            bind_accepted = await rudp.bind(
                capability.fields["capability"], timeout_seconds=timeout_seconds
            )
            return cls(
                meta=meta,
                meta_session=meta_session,
                tcp=tcp,
                rudp=rudp,
                identity=identity,
                welcome=response,
                bind_accepted=bind_accepted,
            )
        except Exception:
            if rudp is not None:
                await rudp.close()
            await tcp.close()
            raise

    async def get_public_meta_json(self, path: str) -> dict[str, Any]:
        return await self._meta.get_json(path, meta_session=self._meta_session)

    async def close(self) -> None:
        await self.rudp.close()
        await self.tcp.close()
