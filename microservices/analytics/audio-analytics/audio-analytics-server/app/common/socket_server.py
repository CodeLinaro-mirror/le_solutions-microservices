# Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
# SPDX-License-Identifier: BSD-3-Clause-Clear

import asyncio
import json
import os
import socket
import threading
from typing import Dict, Any, Callable, Optional
import uuid
from pathlib import Path

from .logger import get_logger

logger = get_logger(__name__)


class SocketServer:
    """
    Unix socket server for direct API-Server communication in blackbox mode.
    Acts as a replacement for Redis pub/sub when BLACKBOX_CONTAINER=true.
    """
    
    def __init__(self, socket_path: str):
        """
        Initialize the socket server.
        
        Args:
            socket_path: Path to the Unix socket file
        """
        self.socket_path = socket_path
        self.server = None
        self.running = False
        self.clients = set()
        self.handlers = {}  # Channel -> handler mapping
        self.subscribers = {}  # Channel -> set of clients
        
        # Ensure socket directory exists
        socket_dir = os.path.dirname(socket_path)
        Path(socket_dir).mkdir(parents=True, exist_ok=True)
        
        # Remove socket file if it exists
        if os.path.exists(socket_path):
            os.unlink(socket_path)
    
    async def start(self):
        """Start the socket server."""
        # Increase buffer limit to handle large messages (e.g., base64 audio files)
        # Max file size is 25MB, base64 encoding increases size by ~33%, so we need ~35MB buffer
        self.server = await asyncio.start_unix_server(
            self._handle_client, self.socket_path, limit=40*1024*1024
        )
        
        # Set socket permissions
        os.chmod(self.socket_path, 0o777)
        
        self.running = True
        logger.info(f"Socket server started on {self.socket_path}")
        
        # Start server in background
        asyncio.create_task(self.server.serve_forever())
    
    async def stop(self):
        """Stop the socket server."""
        if self.server:
            self.running = False
            self.server.close()
            await self.server.wait_closed()
            
            # Close all client connections
            for client in self.clients:
                client.close()
            
            # Remove socket file
            if os.path.exists(self.socket_path):
                os.unlink(self.socket_path)
            
            logger.info("Socket server stopped")
    
    async def _handle_client(self, reader, writer):
        """
        Handle a client connection.
        
        Args:
            reader: StreamReader for reading from client
            writer: StreamWriter for writing to client
        """
        addr = writer.get_extra_info('peername')
        logger.info(f"New client connected: {addr}")
        self.clients.add(writer)
        
        # Store client info for subscriptions
        client_info = {
            'reader': reader,
            'writer': writer,
            'subscriptions': set()
        }
        
        try:
            while self.running:
                # Read data until newline
                data = await reader.readline()
                if not data:
                    break
                
                # Process the message
                try:
                    message = json.loads(data.decode())
                    channel = message.get('channel')
                    msg_data = message.get('message', {})
                    
                    logger.debug(f"Received message on channel {channel}: {msg_data}")
                    
                    # Handle the message (process and publish to output channel)
                    if channel in self.handlers:
                        handler = self.handlers[channel]
                        # Call handler asynchronously - it will publish results
                        asyncio.create_task(self._call_handler(handler, msg_data))
                    else:
                        logger.warning(f"No handler for channel: {channel}")
                
                except json.JSONDecodeError:
                    logger.error(f"Invalid JSON received: {data.decode()}")
                except Exception as e:
                    logger.error(f"Error processing message: {e}", exc_info=True)
        
        except asyncio.CancelledError:
            pass
        except Exception as e:
            logger.error(f"Client connection error: {e}", exc_info=True)
        finally:
            # Clean up subscriptions
            for channel in client_info['subscriptions']:
                if channel in self.subscribers:
                    self.subscribers[channel].discard(writer)
            
            # Clean up
            writer.close()
            self.clients.remove(writer)
            logger.info(f"Client disconnected: {addr}")
    
    async def _call_handler(self, handler, message):
        """
        Call the message handler (no return value expected).
        Handler should publish results to output channels.
        
        Args:
            handler: Message handler function
            message: Message data
        """
        try:
            if asyncio.iscoroutinefunction(handler):
                await handler(json.dumps(message))
            else:
                handler(json.dumps(message))
        except Exception as e:
            logger.error(f"Error in message handler: {e}", exc_info=True)
    
    async def _send_to_client(self, writer, channel, message):
        """
        Send a message to a specific client.
        
        Args:
            writer: StreamWriter for the client
            channel: Channel name
            message: Message data
        """
        try:
            response = {
                'channel': channel,
                'message': message
            }
            writer.write(json.dumps(response).encode() + b'\n')
            await writer.drain()
        except Exception as e:
            logger.error(f"Error sending to client: {e}", exc_info=True)
    
    def register_handler(self, channel: str, handler: Callable):
        """
        Register a handler for a channel.
        
        Args:
            channel: Channel name
            handler: Handler function
        """
        self.handlers[channel] = handler
        logger.info(f"Registered handler for channel: {channel}")
    
    async def publish(self, channel: str, message: str):
        """
        Publish a message to all subscribed clients.
        
        Args:
            channel: Channel name
            message: Message data (JSON string or raw string)
        """
        if not self.running:
            logger.warning("Cannot publish: socket server not running")
            return
        
        try:
            # Try to parse as JSON first
            try:
                msg_data = json.loads(message)
            except json.JSONDecodeError:
                # If not JSON, treat as raw string (e.g., base64 audio data)
                msg_data = message
            
            # Send to all clients (in blackbox mode, all clients get all messages)
            # This mimics Redis pub/sub behavior
            for client in self.clients:
                try:
                    await self._send_to_client(client, channel, msg_data)
                except Exception as e:
                    logger.error(f"Error sending to client: {e}")
        
        except Exception as e:
            logger.error(f"Error publishing message: {e}", exc_info=True)