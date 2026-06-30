# Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
# SPDX-License-Identifier: BSD-3-Clause-Clear

from abc import ABC, abstractmethod
from typing import Dict, Callable, Optional, Any, Union
import asyncio
import json
import os
from .redis_client import RedisClient
from .socket_server import SocketServer
from .communication_factory import CommunicationFactory
from .logger import get_logger

logger = get_logger(__name__)


class BaseService(ABC):
    """
    Abstract base class for all audio analytics services.
    Provides common functionality for Redis pub/sub, message handling, and lifecycle management.
    """
    
    def __init__(self, comm_client: Union[RedisClient, SocketServer], service_name: str):
        """
        Initialize the base service.
        
        Args:
            comm_client: Communication client (Redis or Socket)
            service_name: Name of the service (e.g., "ASR", "T2T", "TTS")
        """
        self.comm_client = comm_client
        self.service_name = service_name
        self.logger = get_logger(f"{__name__}.{service_name}")
        self._running = False
        self._listener_task: Optional[asyncio.Task] = None
        
        # Determine if we're in blackbox mode
        blackbox_mode_str = os.environ.get('BLACKBOX_CONTAINER', 'false').lower()
        self.blackbox_mode = blackbox_mode_str in ('true', '1', 'yes')
        
        if self.blackbox_mode:
            self.logger.info(f"{service_name} service running in BLACKBOX mode (Unix socket communication)")
        else:
            self.logger.info(f"{service_name} service running in standard mode (Redis communication)")
        
    @abstractmethod
    def get_subscriptions(self) -> Dict[str, Callable]:
        """
        Get the channel subscriptions for this service.
        
        Returns:
            Dict mapping channel names to handler functions
        """
        pass
    
    @abstractmethod
    async def initialize(self):
        """
        Initialize the service (load models, setup resources, etc.).
        Called before starting to listen to messages.
        """
        pass
    
    @abstractmethod
    async def cleanup(self):
        """
        Cleanup service resources.
        Called during shutdown.
        """
        pass
    
    async def start(self):
        """Start the service and begin listening to messages."""
        self.logger.info(f'Starting {self.service_name} service...')
        
        # Initialize service
        await self.initialize()
        
        # Subscribe to channels
        subscriptions = self.get_subscriptions()
        pubsub = await CommunicationFactory.subscribe(self.comm_client, subscriptions)
        
        # Start listening (only needed for Redis)
        self._running = True
        if pubsub and not self.blackbox_mode:
            self._listener_task = asyncio.create_task(pubsub.run())
        
        self.logger.info(f'{self.service_name} service started')
    
    async def stop(self):
        """Stop the service and cleanup."""
        self.logger.info(f'Stopping {self.service_name} service...')
        
        self._running = False
        
        # Cancel listener task (Redis only)
        if self._listener_task:
            self._listener_task.cancel()
            try:
                await self._listener_task
            except asyncio.CancelledError:
                pass
        
        # Cleanup service resources
        await self.cleanup()
        
        # Close communication client
        await CommunicationFactory.close(self.comm_client)
        
        self.logger.info(f'{self.service_name} service stopped')
    
    def is_running(self) -> bool:
        """Check if service is running."""
        return self._running
    
    async def publish(self, channel: str, message: str):
        """
        Publish a message to a channel.
        
        Args:
            channel: Channel name
            message: Message to publish (JSON string)
        """
        try:
            await CommunicationFactory.publish(self.comm_client, channel, message)
        except Exception as e:
            self.logger.error(f'Error publishing to {channel}: {e}')
    
    async def publish_json(self, channel: str, data: Dict[str, Any]):
        """
        Publish a JSON message to a Redis channel.
        
        Args:
            channel: Channel name
            data: Data to serialize and publish
        """
        message = json.dumps(data)
        await self.publish(channel, message)
    
    def handle_message_safely(self, handler: Callable):
        """
        Wrap a message handler with error handling.
        
        Args:
            handler: Message handler function
            
        Returns:
            Wrapped handler function
        """
        def wrapped_handler(message):
            try:
                # Extract message data
                if isinstance(message, dict):
                    data = message.get('data')
                else:
                    data = message
                
                # Skip non-message events
                if not isinstance(data, str):
                    return
                
                # Call the actual handler
                return handler(data)
            except Exception as e:
                self.logger.error(f'Error handling message: {e}', exc_info=True)
        
        return wrapped_handler
    
    async def send_error(
        self,
        channel: str,
        error_message: str,
        sync_id: Optional[str] = None,
        session_id: Optional[str] = None,
        param: Optional[str] = None,
        code: Optional[str] = None,
        extra: Optional[dict] = None
    ):
        """
        Send an error message in the standard format.

        Args:
            channel: Channel to send error on
            error_message: Error description
            sync_id: Optional sync ID
            session_id: Optional session ID
            param: Optional parameter that caused the error
            code: Optional error code
            extra: Optional extra fields merged into the result dict
        """
        result = {
            "message": error_message,
            "type": code or "server_error",
            "param": param,
            "code": code
        }
        if extra:
            result.update(extra)

        error_response = {
            "sync_id": sync_id,
            "message_type": "server_error",
            "error": True,
            "result": result
        }
        
        if session_id:
            error_response["session_id"] = session_id
        
        await self.publish(channel, json.dumps(error_response))
        self.logger.error(f'Sent error: {error_message}')
