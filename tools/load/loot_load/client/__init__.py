"""Simulated player clients that use only accepted public boundaries."""

from .session import BoundaryAuthenticationRejected, BoundaryGameClient

__all__ = ["BoundaryAuthenticationRejected", "BoundaryGameClient"]
