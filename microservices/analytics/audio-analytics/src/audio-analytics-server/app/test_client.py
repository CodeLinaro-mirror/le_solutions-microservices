#!/usr/bin/env python3
# Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
# SPDX-License-Identifier: BSD-3-Clause-Clear

"""
Simple test client to verify the Audio Analytics Server is working.
This script sends test messages to the server and listens for responses.
"""

import asyncio
import redis.asyncio as redis
import json
import base64
import sys
from common.config import Config
from models.messages import (
    TranscriptionsCreateRequest,
    TranslationRequest,
    TTSSynthesizeRequest
)


class TestClient:
    """Simple test client for Audio Analytics Server."""
    
    def __init__(self):
        self.redis_client = None
        
    async def connect(self):
        """Connect to Redis."""
        print(f'Connecting to Redis at {Config.REDIS_HOST}:{Config.REDIS_PORT}...')
        self.redis_client = redis.Redis(
            host=Config.REDIS_HOST,
            port=Config.REDIS_PORT,
            decode_responses=True
        )
        await self.redis_client.ping()
        print('Connected to Redis')
    
    async def close(self):
        """Close Redis connection."""
        if self.redis_client:
            await self.redis_client.aclose()
    
    async def test_asr_file_sync(self):
        """Test ASR with file-based synchronous transcription."""
        print('\n' + '='*60)
        print('TEST: ASR File-Based Synchronous Transcription')
        print('='*60)
        
        # Create a mock audio file (just some dummy data)
        mock_audio = b'\x00\x01\x02\x03' * 100
        audio_base64 = base64.b64encode(mock_audio).decode('utf-8')
        
        # Create request
        request = TranscriptionsCreateRequest(
            model="whisper-1",
            language="en",
            stream=False,
            file=audio_base64
        )
        
        print(f'Sending request with sync_id: {request.sync_id}')
        
        # Subscribe to output channel
        pubsub = self.redis_client.pubsub()
        await pubsub.subscribe(Config.ASR_TRANSCRIPTION_OUT)
        
        # Publish request
        await self.redis_client.publish(Config.ASR_TRANSCRIPTION_IN, request.to_json())
        print('Request sent, waiting for response...')
        
        # Wait for response
        timeout = 10
        start_time = asyncio.get_event_loop().time()
        
        async for message in pubsub.listen():
            if message['type'] == 'message':
                data = json.loads(message['data'])
                print(f'Received response: {json.dumps(data, indent=2)}')
                break
            
            if asyncio.get_event_loop().time() - start_time > timeout:
                print('Timeout waiting for response')
                break
        
        await pubsub.unsubscribe(Config.ASR_TRANSCRIPTION_OUT)
        await pubsub.close()
    
    async def test_asr_models(self):
        """Test ASR models list request."""
        print('\n' + '='*60)
        print('TEST: ASR Models List')
        print('='*60)
        
        from models.messages import TranscriptionsModelsRequest
        
        request = TranscriptionsModelsRequest()
        print(f'Requesting models list with sync_id: {request.sync_id}')
        
        # Subscribe to models channel
        pubsub = self.redis_client.pubsub()
        await pubsub.subscribe(Config.ASR_MODELS)
        
        # Publish request
        await self.redis_client.publish(Config.ASR_MODELS, request.to_json())
        print('Request sent, waiting for response...')
        
        # Wait for response
        timeout = 5
        start_time = asyncio.get_event_loop().time()
        
        async for message in pubsub.listen():
            if message['type'] == 'message':
                data = json.loads(message['data'])
                print(f'Available models: {data.get("models", [])}')
                break
            
            if asyncio.get_event_loop().time() - start_time > timeout:
                print('Timeout waiting for response')
                break
        
        await pubsub.unsubscribe(Config.ASR_MODELS)
        await pubsub.close()
    
    async def test_t2t_translation(self):
        """Test T2T translation."""
        print('\n' + '='*60)
        print('TEST: T2T Translation')
        print('='*60)
        
        request = TranslationRequest(
            text=["Hello world", "How are you?"],
            model="translation-model-1",
            source_language="en",
            target_language="fr"
        )
        
        print(f'Translating: {request.text}')
        print(f'From {request.source_language} to {request.target_language}')
        
        # Subscribe to output channel
        pubsub = self.redis_client.pubsub()
        await pubsub.subscribe(Config.T2T_TRANSLATION_OUT)
        
        # Publish request
        await self.redis_client.publish(Config.T2T_TRANSLATION_IN, request.to_json())
        print('Request sent, waiting for response...')
        
        # Wait for response
        timeout = 5
        start_time = asyncio.get_event_loop().time()
        
        async for message in pubsub.listen():
            if message['type'] == 'message':
                data = json.loads(message['data'])
                print(f'Translation results:')
                for translation in data.get('result', {}).get('translations', []):
                    print(f'  - {translation["translated_text"]}')
                break
            
            if asyncio.get_event_loop().time() - start_time > timeout:
                print('Timeout waiting for response')
                break
        
        await pubsub.unsubscribe(Config.T2T_TRANSLATION_OUT)
        await pubsub.close()
    
    async def test_tts_synthesis(self):
        """Test TTS synthesis."""
        print('\n' + '='*60)
        print('TEST: TTS Synthesis')
        print('='*60)
        
        request = TTSSynthesizeRequest(
            text="Hello, this is a test of text to speech.",
            model="tts-model-1",
            language="en",
            voice="default"
        )
        
        print(f'Synthesizing: "{request.text}"')
        
        # Subscribe to output channel
        pubsub = self.redis_client.pubsub()
        await pubsub.subscribe(Config.TTS_AUDIO_OUT)
        
        # Publish request
        await self.redis_client.publish(Config.TTS_TEXT_IN, request.to_json())
        print('Request sent, waiting for audio chunks...')
        
        # Wait for audio chunks and completion
        timeout = 10
        start_time = asyncio.get_event_loop().time()
        chunk_count = 0
        
        async for message in pubsub.listen():
            if message['type'] == 'message':
                data = message['data']
                
                # Check if it's the completion message (JSON)
                if data.startswith('{'):
                    completion = json.loads(data)
                    if completion.get('status') == 'done':
                        print(f'Synthesis complete! Received {chunk_count} audio chunks')
                        break
                else:
                    # Audio chunk (binary data)
                    chunk_count += 1
                    print(f'Received audio chunk {chunk_count} ({len(data)} bytes)')
            
            if asyncio.get_event_loop().time() - start_time > timeout:
                print('Timeout waiting for completion')
                break
        
        await pubsub.unsubscribe(Config.TTS_AUDIO_OUT)
        await pubsub.close()
    
    async def run_all_tests(self):
        """Run all tests."""
        try:
            await self.connect()
            
            # Run tests
            await self.test_asr_models()
            await self.test_asr_file_sync()
            await self.test_t2t_translation()
            await self.test_tts_synthesis()
            
            print('\n' + '='*60)
            print('All tests completed!')
            print('='*60)
            
        except Exception as e:
            print(f'Error running tests: {e}', file=sys.stderr)
            import traceback
            traceback.print_exc()
        finally:
            await self.close()


async def main():
    """Main entry point."""
    print('Audio Analytics Server - Test Client')
    print('='*60)
    
    client = TestClient()
    await client.run_all_tests()


if __name__ == '__main__':
    asyncio.run(main())
