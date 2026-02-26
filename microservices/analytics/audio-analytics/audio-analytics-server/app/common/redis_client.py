# Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
# SPDX-License-Identifier: BSD-3-Clause-Clear

import redis.asyncio as redis
import asyncio
from typing import Callable, Dict, Optional, Any
import json
from .config import Config
from .logger import get_logger

logger = get_logger(__name__)


class RedisClient:
    """
    Redis client wrapper for pub/sub operations.
    Handles connection management and provides helper methods.
    """
    
    def __init__(self, host: Optional[str] = None, port: Optional[int] = None):
        """
        Initialize Redis client.
        
        Args:
            host: Redis host (defaults to Config.REDIS_HOST)
            port: Redis port (defaults to Config.REDIS_PORT)
        """
        self.host = host or Config.REDIS_HOST
        self.port = port or Config.REDIS_PORT
        self.client: Optional[redis.Redis] = None
        self.pubsub: Optional[redis.client.PubSub] = None
        self._sync_responses: Dict[str, asyncio.Future] = {}
        
    async def connect(self) -> redis.Redis:
        """
        Connect to Redis and configure.
        
        Returns:
            Redis client instance
        """
        logger.info(f'Connecting to Redis at {self.host}:{self.port}...')
        
        self.client = redis.Redis(
            host=self.host,
            port=self.port,
            decode_responses=True
        )
        
        # Test connection
        await self.ping()
        
        # Set notify-keyspace-events configuration
        await self.client.config_set('notify-keyspace-events', 'KEA')
        logger.info(f'Redis config: {await self.client.config_get("notify-keyspace-events")}')
        
        return self.client
    
    async def ping(self) -> bool:
        """
        Ping Redis to check connection.
        
        Returns:
            True if ping successful
        """
        if not self.client:
            raise RuntimeError("Redis client not connected")
        
        result = await self.client.ping()
        logger.info(f'Redis ping: {result}')
        return result
    
    async def publish(self, channel: str, message: str) -> int:
        """
        Publish a message to a Redis channel.
        
        Args:
            channel: Channel name
            message: Message to publish (JSON string)
            
        Returns:
            Number of subscribers that received the message
        """
        if not self.client:
            raise RuntimeError("Redis client not connected")
        
        logger.debug(f'Publishing to {channel}: {message[:100]}...')
        return await self.client.publish(channel, message)
    
    async def subscribe(self, channels: Dict[str, Callable]) -> redis.client.PubSub:
        """
        Subscribe to Redis channels with handlers.
        
        Args:
            channels: Dict mapping channel names to handler functions
            
        Returns:
            PubSub instance
        """
        if not self.client:
            raise RuntimeError("Redis client not connected")
        
        self.pubsub = self.client.pubsub()
        await self.pubsub.subscribe(**channels)
        
        logger.info(f'Subscribed to channels: {list(channels.keys())}')
        return self.pubsub
    
    async def unsubscribe(self, *channels: str):
        """
        Unsubscribe from Redis channels.
        
        Args:
            channels: Channel names to unsubscribe from
        """
        if self.pubsub:
            await self.pubsub.unsubscribe(*channels)
            logger.info(f'Unsubscribed from channels: {list(channels)}')
    
    async def close(self):
        """Close Redis connection and cleanup."""
        if self.pubsub:
            await self.pubsub.close()
            logger.info('Closed Redis pubsub')
        
        if self.client:
            await self.client.aclose()
            logger.info('Closed Redis client')
    
    async def request_response(
        self,
        request_channel: str,
        response_channel: str,
        message: str,
        sync_id: str,
        timeout: float = 30.0
    ) -> Dict[str, Any]:
        """
        Send a request and wait for a response (synchronous pattern over pub/sub).
        
        Args:
            request_channel: Channel to send request on
            response_channel: Channel to listen for response on
            message: Request message (JSON string)
            sync_id: Synchronization ID to match request/response
            timeout: Timeout in seconds
            
        Returns:
            Response message as dict
            
        Raises:
            asyncio.TimeoutError: If response not received within timeout
        """
        # Create a future for this request
        future = asyncio.Future()
        self._sync_responses[sync_id] = future
        
        # Publish request
        await self.publish(request_channel, message)
        
        try:
            # Wait for response with timeout
            response = await asyncio.wait_for(future, timeout=timeout)
            return response
        finally:
            # Cleanup
            self._sync_responses.pop(sync_id, None)
    
    def complete_sync_request(self, sync_id: str, response: Dict[str, Any]):
        """
        Complete a synchronous request with a response.
        
        Args:
            sync_id: Synchronization ID
            response: Response data
        """
        future = self._sync_responses.get(sync_id)
        if future and not future.done():
            future.set_result(response)
            logger.debug(f'Completed sync request {sync_id}')


async def create_redis_client(host: Optional[str] = None, port: Optional[int] = None) -> RedisClient:
    """
    Create and connect a Redis client.
    
    Args:
        host: Redis host
        port: Redis port
        
    Returns:
        Connected RedisClient instance
    """
    client = RedisClient(host, port)
    await client.connect()
    return client
