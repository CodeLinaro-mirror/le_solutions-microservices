# Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
# SPDX-License-Identifier: BSD-3-Clause-Clear

"""
Audio Analytics Server - Main Entry Point

This is the main orchestrator that initializes and manages all audio analytics services:
- ASR (Automatic Speech Recognition)
- T2T (Text-to-Text Translation)
- TTS (Text-to-Speech)

All services communicate via Redis pub/sub channels.
"""

import asyncio
import signal
import sys
from typing import List

import os
from common.config import Config
from common.logger import setup_logger, get_logger
from common.redis_client import create_redis_client, RedisClient
from common.socket_server import SocketServer
from common.communication_factory import CommunicationFactory
from services import ASRService, T2TService, TTSService
from common.base_service import BaseService

# Setup logging
setup_logger(__name__)
logger = get_logger(__name__)


class AudioAnalyticsServer:
    """
    Main server orchestrator for all audio analytics services.
    """
    
    def __init__(self):
        self.redis_client: RedisClient = None
        self.services: List[BaseService] = []
        self.running = False
        logger.info('=================================================================')
        logger.info('   NEW SESSION')
        logger.info('=================================================================')
        
    async def initialize(self):
        """Initialize communication client and all services."""
        logger.info('Initializing Audio Analytics Server...')
        
        # Determine if we're in blackbox mode
        blackbox_mode_str = os.environ.get('BLACKBOX_CONTAINER', 'false').lower()
        blackbox_mode = blackbox_mode_str in ('true', '1', 'yes')
        
        # Create appropriate communication client
        self.comm_client = await CommunicationFactory.create_client(blackbox_mode)    

        # Initialize services
        logger.info('Initializing services...')
        self.services = [
            ASRService(self.comm_client),
            T2TService(self.comm_client),
            TTSService(self.comm_client)
        ]
        
        if not blackbox_mode:
            logger.info(f'Redis: {Config.REDIS_HOST}:{Config.REDIS_PORT}')
            
        logger.info(f'Initialized {len(self.services)} services')
    
    async def start(self):
        """Start all services."""
        logger.info('Starting Audio Analytics Server...')
        self.running = True
        
        # Start all services
        for service in self.services:
            await service.start()
        
        logger.info('Audio Analytics Server started successfully')
        logger.info(f'Listening on channels:')
        logger.info(f'  ASR: {Config.ASR_TRANSCRIPTION_IN}, {Config.ASR_MODELS}')
        logger.info(f'  T2T: {Config.T2T_TRANSLATION_IN}, {Config.T2T_MODELS}')
        logger.info(f'  TTS: {Config.TTS_TEXT_IN}, {Config.TTS_MODELS}')
    
    async def stop(self):
        """Stop all services and cleanup."""
        logger.info('Stopping Audio Analytics Server...')
        self.running = False
        
        # Stop all services
        for service in self.services:
            try:
                await service.stop()
            except Exception as e:
                logger.error(f'Error stopping service: {e}')
        
        logger.info('Audio Analytics Server stopped')
    
    async def run(self):
        """Run the server until interrupted."""
        try:
            await self.initialize()
            await self.start()
            
            # Keep running until interrupted
            while self.running:
                await asyncio.sleep(1)
                
        except asyncio.CancelledError:
            logger.info('Server cancelled, shutting down...')
        except Exception as e:
            logger.error(f'Server error: {e}', exc_info=True)
        finally:
            await self.stop()


async def async_main():
    """Main async entry point."""
    server = AudioAnalyticsServer()
    
    # Setup signal handlers for graceful shutdown
    loop = asyncio.get_event_loop()
    
    def signal_handler():
        logger.info('Received shutdown signal')
        asyncio.create_task(server.stop())
    
    # Register signal handlers
    for sig in (signal.SIGTERM, signal.SIGINT):
        loop.add_signal_handler(sig, signal_handler)
    
    # Run server
    await server.run()


def main():
    """Main entry point."""
    logger.info('='*60)
    logger.info('Audio Analytics Server')
    logger.info('='*60)       
    logger.info(f'Log Level: {Config.LOG_LEVEL}')
    logger.info('='*60)
    
    try:
        asyncio.run(async_main())
    except KeyboardInterrupt:
        logger.info('Interrupted by user')
    except Exception as e:
        logger.error(f'Fatal error: {e}', exc_info=True)
        sys.exit(1)
    
    logger.info('Exiting Audio Analytics Server')


if __name__ == "__main__":
    main()
