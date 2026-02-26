# Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
# SPDX-License-Identifier: BSD-3-Clause-Clear

import os
import asyncio
from typing import Dict, Callable, Optional, Any, Union

from .redis_client import RedisClient, create_redis_client
from .socket_server import SocketServer
from .logger import get_logger

logger = get_logger(__name__)


class CommunicationFactory:
    """
    Factory for creating the appropriate communication mechanism
    based on the BLACKBOX_CONTAINER environment variable.
    """
    
    @staticmethod
    async def create_client(blackbox_mode: Optional[bool] = None) -> Union[RedisClient, SocketServer]:
        """
        Create a communication client based on the environment.
        
        Args:
            blackbox_mode: Override the environment variable
            
        Returns:
            Either a RedisClient or SocketServer
        """
        # Determine mode from environment if not specified
        if blackbox_mode is None:
            blackbox_mode_str = os.environ.get('BLACKBOX_CONTAINER', 'false').lower()
            blackbox_mode = blackbox_mode_str in ('true', '1', 'yes')
        
        if blackbox_mode:
            logger.info("Using BLACKBOX mode with Unix socket communication")
            socket_path = os.environ.get('SOCKET_PATH', '/tmp/audio-sockets/audio-analytics.sock')
            socket_server = SocketServer(socket_path)
            await socket_server.start()
            return socket_server
        else:
            logger.info("Using Redis communication")
            redis_host = os.environ.get('REDIS_HOST', 'redis')
            redis_port = int(os.environ.get('REDIS_PORT', 6379))
            return await create_redis_client(redis_host, redis_port)
    
    @staticmethod
    async def subscribe(
        client: Union[RedisClient, SocketServer],
        channels: Dict[str, Callable]
    ) -> Optional[Any]:
        """
        Subscribe to channels using the appropriate client.
        
        Args:
            client: Communication client (Redis or Socket)
            channels: Dict mapping channel names to handler functions
            
        Returns:
            PubSub instance for Redis, None for Socket
        """
        if isinstance(client, RedisClient):
            return await client.subscribe(channels)
        else:
            # Register handlers with socket server
            for channel, handler in channels.items():
                client.register_handler(channel, handler)
            return None
    
    @staticmethod
    async def publish(
        client: Union[RedisClient, SocketServer],
        channel: str,
        message: str
    ) -> None:
        """
        Publish a message using the appropriate client.
        
        Args:
            client: Communication client (Redis or Socket)
            channel: Channel name
            message: Message to publish (JSON string)
        """
        if isinstance(client, RedisClient):
            await client.publish(channel, message)
        else:
            await client.publish(channel, message)
    
    @staticmethod
    async def close(client: Union[RedisClient, SocketServer]) -> None:
        """
        Close the communication client.
        
        Args:
            client: Communication client (Redis or Socket)
        """
        if isinstance(client, RedisClient):
            await client.close()
        else:
            await client.stop()