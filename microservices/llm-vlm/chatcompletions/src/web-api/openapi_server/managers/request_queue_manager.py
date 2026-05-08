# Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
# SPDX-License-Identifier: BSD-3-Clause-Clear

"""
RequestQueueManager: Session-aware queue for serializing DSP access in ADHOC_MODE.

Core Guarantee: Only ONE event can access DSP at any given time.

This is enforced through:
1. Atomic lock acquisition (active_lock)
2. Session tracking (active_session_id)
3. Event tracking (active_event_id)
4. Priority queue for waiting requests
5. Session architecture (one active event per session)

The queue only operates in ADHOC_MODE. In normal mode, requests bypass the queue
for optimal performance.
"""

import asyncio
import os
import threading
import time
from typing import Optional, Dict, Any

from fastapi import HTTPException

from openapi_server.impl.event_based_chat_handler import EventBasedChatHandler
from openapi_server.managers.session_manager import SessionManager
from openapi_server.impl.constant import ADHOC_MODE
from openapi_server.events.conversation_event import EventState
from openapi_server.logger.logger_config import LoggerConfig

LoggerConfig.initialize()
logger = LoggerConfig.get_logger(__name__)


# Queue Configuration
MAX_QUEUE_SIZE = 50  # 2x expected concurrent requests (25) for buffer
REQUEST_TIMEOUT = 300  # 5 minutes
CLEANUP_DELAY = 0.5  # seconds between requests for resource cleanup

# Watchdog: maximum silence (no token produced by the subprocess) before the
# session is considered unresponsive and the DSP lock is force-released.
# A model that is legitimately generating a long response keeps updating the
# heartbeat on every token, so the watchdog never fires for it.
# Configurable via TOKEN_SILENCE_TIMEOUT env var (seconds). Default: 180s.
TOKEN_SILENCE_TIMEOUT = int(os.getenv("TOKEN_SILENCE_TIMEOUT", "180"))
LOCK_WATCHDOG_INTERVAL = 15  # check every 15 seconds

# Priority Levels
PRIORITY_TOOL_CONTINUATION = 0  # Highest - tool responses
PRIORITY_NORMAL = 1  # Normal - new requests


class RequestQueueManager:
    """
    Session-aware queue manager for ADHOC_MODE.

    Key Invariant: Only ONE session can have an ACTIVE event at any time.

    Rules:
    1. If no active event → process any request
    2. If active event from Session A → only Session A requests can proceed
    3. All other sessions must wait in queue
    4. Tool continuations get priority in queue
    """

    _instance = None
    _lock = threading.Lock()

    def __new__(cls):
        if cls._instance is None:
            with cls._lock:
                if cls._instance is None:
                    cls._instance = super().__new__(cls)
                    cls._instance._initialized = False
        return cls._instance

    def __init__(self):
        """Initialize the RequestQueueManager singleton."""
        if not self._initialized:
            # Priority queue for waiting requests
            self.request_queue = asyncio.PriorityQueue(maxsize=MAX_QUEUE_SIZE)

            # Active event tracking
            self.active_session_id: Optional[str] = None  # Which session is executing
            self.active_event_id: Optional[str] = None  # Which event is active
            self.active_lock = asyncio.Lock()  # Protects active state

            # Worker task
            self.worker_task: Optional[asyncio.Task] = None
            self.watchdog_task: Optional[asyncio.Task] = None
            self.enabled = ADHOC_MODE
            self._request_counter = 0

            # Heartbeat timestamp: updated on every token produced by the active
            # subprocess.  Initialized when the lock is acquired.  The watchdog
            # fires when no token has been seen for TOKEN_SILENCE_TIMEOUT seconds.
            self._last_token_at: Optional[float] = None

            self._initialized = True
            logger.info(f"RequestQueueManager initialized (enabled={self.enabled})")

    @classmethod
    def get_instance(cls) -> 'RequestQueueManager':
        """Get the singleton instance."""
        return cls()

    async def enqueue_request(self, request_data, raw_json, session):
        """
        Enqueue request with pre-resolved session.

        Flow:
        1. Use pre-resolved session from API layer
        2. Check if this session can proceed immediately (atomic)
        3. If yes, process immediately
        4. If no, queue and wait

        Args:
            request_data: The chat completion request
            raw_json: Optional raw JSON to bypass Pydantic issues
            session: Pre-resolved ConversationSession from API layer

        Returns:
            Chat completion response or streaming response
        """
        if not self.enabled:
            # Normal mode - bypass queue for zero overhead
            return await EventBasedChatHandler.handle_chat_completion(
                request_data, raw_json, session
            )

        # Use pre-resolved session
        session_id = session.session_id

        # DEBUG: Check if this is a tool continuation
        messages = EventBasedChatHandler.extract_messages_from_request(
            request_data, raw_json
        )
        is_tool_continuation = self._is_tool_continuation(messages)

        # Step 2: Atomic check and acquire
        async with self.active_lock:
            can_proceed = (
                self.active_session_id is None or  # No active event
                self.active_session_id == session_id  # Same session continuing
            )

            # DEBUG: Log lock acquisition decision
            if self.active_session_id is None:
                logger.info(f"🔓 No active session - {session_id} can proceed")
            elif self.active_session_id == session_id:
                logger.info(f"🔄 Same session continuing - {session_id} can proceed (tool_continuation={is_tool_continuation})")
            else:
                logger.info(f"🚫 Different session active - {session_id} must queue (active={self.active_session_id}, tool_continuation={is_tool_continuation})")

            if can_proceed:
                # Immediately mark as active BEFORE releasing lock
                self.active_session_id = session_id
                self._last_token_at = time.time()  # initialise heartbeat
                logger.info(f"🔒 Session {session_id} acquired DSP lock")

        # Step 3: Process or queue
        if can_proceed:
            # Fast path - process immediately with lock already acquired
            try:
                result = await self._process_request_with_lock_held(
                    request_data, raw_json, session
                )
                return result
            except Exception as e:
                # Release lock on error
                async with self.active_lock:
                    if self.active_session_id == session_id:
                        logger.error(f"❌ Error in fast path, releasing lock: {e}")
                        self.active_session_id = None
                        self.active_event_id = None
                raise
        else:
            # Must queue - different session has active event
            return await self._queue_and_wait(
                request_data, raw_json, session
            )

    async def _resolve_session_id(self, request_data, raw_json) -> str:
        """
        Resolve which session this request belongs to.
        Uses the same logic as EventBasedChatHandler.

        Args:
            request_data: The chat completion request
            raw_json: Optional raw JSON

        Returns:
            Session ID (chat completion ID)
        """
        user_id = getattr(request_data, 'user', 'default_user')
        messages = EventBasedChatHandler.extract_messages_from_request(
            request_data, raw_json
        )

        session_manager = SessionManager.get_instance()
        session, is_new, _ = session_manager.find_or_create_session(
            user_id,
            messages,
            request_data,
            raw_json
        )

        return session.session_id

    async def _process_request_with_lock_held(self, request_data, raw_json, session):
        """
        Process request with pre-resolved session. Lock is already held (active_session_id is set).

        Uses unified callback approach for both streaming and non-streaming:
        - Registers completion callback with event
        - Callback checks event state and releases lock if COMPLETED
        - Keeps lock if event is ACTIVE (tool calling)
        - Waits for callback to complete before returning to ensure HTTP response is sent

        Args:
            request_data: The chat completion request
            raw_json: Optional raw JSON
            session: Pre-resolved ConversationSession

        Returns:
            Chat completion response or streaming response
        """
        session_id = session.session_id

        try:
            # Create callback for lock management
            callback_done = asyncio.Event()

            async def handle_event_completion(event_id, event_state):
                try:
                    async with self.active_lock:
                        # Only release if this session still holds the lock
                        if self.active_session_id == session_id:
                            if event_state in (EventState.COMPLETED, EventState.FAILED, EventState.CANCELLED):
                                logger.info(
                                    f"✅ Event {event_id} finished with state {event_state.name} (callback) - releasing lock"
                                )
                                self.active_session_id = None
                                self.active_event_id = None
                                self._last_token_at = None
                            else:
                                # Event still active (shouldn't happen in callback, but handle it)
                                logger.info(
                                    f"🔄 Event {event_id} still {event_state.name} (callback) - keeping lock"
                                )
                finally:
                    # Signal that callback has completed
                    callback_done.set()

            # Execute request with pre-resolved session and callback
            # The callback will be registered on the event BEFORE execute_turn() is called
            result = await EventBasedChatHandler.handle_chat_completion(
                request_data, raw_json, session, completion_callback=handle_event_completion
            )

            # Handle callback completion based on response type
            if session.current_event:
                event = session.current_event
                self.active_event_id = event.event_id

                # Check if result is a StreamingResponse
                # For streaming, callback will be triggered from the generator
                # For non-streaming, we need to handle it here
                from fastapi.responses import StreamingResponse
                is_streaming = isinstance(result, StreamingResponse)

                if not is_streaming:
                    # CRITICAL FIX: Handle race condition for non-streaming requests
                    # Event may have already completed before we registered the callback
                    if event.is_completed():
                        logger.info(f"⚡ Event {event.event_id} already completed - triggering callback immediately")
                        await handle_event_completion(event.event_id, event.state)
                        # Wait for callback to complete before returning
                        await callback_done.wait()
                        logger.info(f"🔓 Callback completed for event {event.event_id} - safe to return response")
                    elif event.is_active():
                        # TOOL CALLING FIX: Event is ACTIVE (tool calling in progress)
                        # Don't wait for callback - return immediately so client receives tool_calls
                        # Lock will be kept until tool response completes the turn
                        logger.info(f"🔧 Event {event.event_id} is ACTIVE (tool calling) - returning immediately, lock kept")
                    else:
                        # Event is in some other state - wait for callback as safety measure
                        logger.warning(f"⚠️  Event {event.event_id} in unexpected state {event.state} - waiting for callback")
                        await callback_done.wait()
                        logger.info(f"🔓 Callback completed for event {event.event_id}")
                else:
                    # For streaming, don't wait
                    # - LLM streaming: callback will be triggered from generator's finally block
                    # - VLM streaming: callback already triggered after subprocess completion
                    logger.info(f"🌊 Streaming response - callback handled by streaming implementation")
            else:
                # No event created (error case) - release lock
                async with self.active_lock:
                    logger.warning(f"⚠️  No current event for session {session_id}")
                    self.active_session_id = None
                    self.active_event_id = None
                    self._last_token_at = None

            return result

        except asyncio.CancelledError:
            # Always release lock on cancellation
            async with self.active_lock:
                if self.active_session_id == session_id:
                    logger.warning(f"⚠️  Request cancelled, releasing lock")
                    self.active_session_id = None
                    self.active_event_id = None
                    self._last_token_at = None
            raise

        except Exception as e:
            # Always release lock on error
            async with self.active_lock:
                if self.active_session_id == session_id:
                    logger.error(f"❌ Error processing request, releasing lock: {e}")
                    self.active_session_id = None
                    self.active_event_id = None
                    self._last_token_at = None

            # DSP initialization failure: kill the subprocess and drain the queue
            # so all waiting clients get an immediate user-friendly error instead
            # of queuing up and failing one-by-one.
            if self._is_dsp_init_failure(e):
                await self._handle_dsp_failure(e, session_id)

            raise

    async def _queue_and_wait(self, request_data, raw_json, session):
        """
        Queue request with pre-resolved session and wait for processing.

        Args:
            request_data: The chat completion request
            raw_json: Optional raw JSON
            session: Pre-resolved ConversationSession

        Returns:
            Chat completion response or streaming response
        """
        session_id = session.session_id

        logger.info(
            f"📥 Queueing request for session {session_id} "
            f"(active_session={self.active_session_id})"
        )

        # Determine priority
        messages = EventBasedChatHandler.extract_messages_from_request(
            request_data, raw_json
        )
        is_tool_continuation = self._is_tool_continuation(messages)
        priority = PRIORITY_TOOL_CONTINUATION if is_tool_continuation else PRIORITY_NORMAL

        # Create queue item
        future = asyncio.Future()
        queue_item = {
            'priority': priority,
            'counter': self._request_counter,
            'request_data': request_data,
            'raw_json': raw_json,
            'session': session,  # Store session object
            'future': future,
            'timestamp': time.time(),
            'is_tool_continuation': is_tool_continuation
        }

        self._request_counter += 1

        try:
            # Add to queue (non-blocking with timeout)
            await asyncio.wait_for(
                self.request_queue.put((priority, queue_item['counter'], queue_item)),
                timeout=5.0
            )

            queue_size = self.request_queue.qsize()
            priority_str = "TOOL_CONTINUATION" if is_tool_continuation else "NORMAL"
            logger.info(f"📊 Queue size: {queue_size} (priority={priority_str})")

        except asyncio.TimeoutError:
            logger.error("Queue is full - rejecting request")
            raise HTTPException(
                status_code=503,
                detail="Service temporarily unavailable - queue full"
            )

        # Wait for result with timeout
        try:
            result = await asyncio.wait_for(future, timeout=REQUEST_TIMEOUT)
            return result

        except asyncio.TimeoutError:
            logger.error(f"Request timed out after {REQUEST_TIMEOUT}s in queue")
            raise HTTPException(
                status_code=504,
                detail=f"Request timed out after {REQUEST_TIMEOUT} seconds"
            )

    def _is_tool_continuation(self, messages) -> bool:
        """
        Detect if this is a tool continuation request.

        Tool continuations have the pattern: [..., assistant(tool_calls), tool]

        Args:
            messages: List of message dictionaries

        Returns:
            True if this is a tool continuation, False otherwise
        """
        if len(messages) < 2:
            return False

        # Check last message is tool response
        if messages[-1].get('role') != 'tool':
            return False

        # Check second-to-last is assistant with tool_calls
        if messages[-2].get('role') == 'assistant':
            has_tool_calls = messages[-2].get('tool_calls') is not None
            return has_tool_calls

        return False

    async def _worker(self):
        """
        Background worker that processes priority queue sequentially.

        Only processes requests when no active event exists,
        or when the request is for the currently active session.

        This ensures only ONE event accesses DSP at any time.
        """
        logger.info("🚀 RequestQueueManager worker started")

        while True:
            try:
                # Get next request from priority queue
                priority, counter, item = await self.request_queue.get()

                request_data = item['request_data']
                raw_json = item['raw_json']
                session = item['session']  # Get session object
                future = item['future']
                timestamp = item['timestamp']
                is_tool_continuation = item['is_tool_continuation']

                session_id = session.session_id

                # Check if request has already timed out or was cancelled by client
                wait_time = time.time() - timestamp
                if wait_time > REQUEST_TIMEOUT:
                    logger.warning(f"⏱️  Request expired in queue (waited {wait_time:.1f}s)")
                    if not future.done():
                        future.set_exception(
                            HTTPException(504, "Request expired in queue")
                        )
                    self.request_queue.task_done()
                    continue

                if future.done():
                    logger.warning(f"⚠️  Request already cancelled or completed, skipping (waited {wait_time:.1f}s)")
                    self.request_queue.task_done()
                    continue

                # Wait until we can process this request
                # (either no active event, or same session)
                while True:
                    async with self.active_lock:
                        can_proceed = (
                            self.active_session_id is None or
                            self.active_session_id == session_id
                        )

                        if can_proceed:
                            # Acquire lock for this session
                            self.active_session_id = session_id
                            self._last_token_at = time.time()  # initialise heartbeat
                            break

                    # Can't proceed yet - wait a bit
                    await asyncio.sleep(0.1)

                # Process the request
                priority_str = "TOOL_CONTINUATION" if is_tool_continuation else "NORMAL"
                logger.info(
                    f"🔄 Processing queued request for session {session_id} "
                    f"(priority={priority_str}, waited={wait_time:.1f}s)"
                )

                try:
                    result = await self._process_request_with_lock_held(
                        request_data, raw_json, session
                    )

                    if not future.done():
                        future.set_result(result)

                    logger.info(f"✅ Completed queued request for session {session_id}")

                except Exception as e:
                    logger.error(f"❌ Error processing queued request: {e}", exc_info=True)
                    if not future.done():
                        future.set_exception(e)

                finally:
                    self.request_queue.task_done()

                    # Small delay for cleanup (especially important in ADHOC_MODE)
                    await asyncio.sleep(CLEANUP_DELAY)

            except asyncio.CancelledError:
                logger.info("Worker task cancelled - shutting down")
                break

            except Exception as e:
                logger.error(f"Worker error: {e}", exc_info=True)
                await asyncio.sleep(1)  # Prevent tight error loop

    @staticmethod
    def _is_dsp_init_failure(error: Exception) -> bool:
        """
        Return True when the error indicates a DSP/NPU initialization failure.

        These errors mean the hardware is in a bad state and every subsequent
        request will fail with the same error until the DSP is freed.
        """
        msg = str(error).lower()
        return any(keyword in msg for keyword in (
            "process failed to initialize",
            "failed to create llm handle",
            "failed to create vlm handle",
            "failed to create the dialog",
            "sdk code",
            "device creation failure",
        ))

    def update_inference_heartbeat(self):
        """
        Record that the active subprocess just produced output (a token).

        Called by streaming generators on every token and by non-streaming
        handlers when the first token arrives.  Resets the silence timer so
        the watchdog does not consider the subprocess unresponsive.
        """
        self._last_token_at = time.time()

    async def _handle_dsp_failure(self, error: Exception, failed_session_id: str):
        """
        Handle a DSP/NPU initialization failure:
        1. Force-kill the failed subprocess to free DSP resources for recovery.
        2. Drain all queued requests with a user-friendly 503 so clients can
           retry immediately rather than waiting in a queue that will keep failing.
        """
        logger.error(
            f"🔴 DSP initialization failure for session {failed_session_id} — "
            "killing subprocess and draining queue"
        )

        # Force-kill the subprocess to free DSP/NPU resources
        try:
            from openapi_server.managers.session_manager import SessionManager
            session = SessionManager.get_instance().get_session(failed_session_id)
            if session:
                if session.current_event and session.current_event.is_active():
                    logger.info(
                        f"🔴 Force-killing subprocess for event "
                        f"{session.current_event.event_id}"
                    )
                    session.current_event.terminate_handle(force=True)
                elif session.events:
                    last_event = session.events[-1]
                    logger.info(
                        f"🔴 Force-killing subprocess for last event {last_event.event_id}"
                    )
                    last_event.terminate_handle(force=True)
        except Exception as kill_err:
            logger.error(f"🔴 Error killing subprocess for {failed_session_id}: {kill_err}")

        # Drain all queued requests with a user-friendly message
        user_message = "Service temporarily unavailable. Please try again in a moment."
        drained = 0
        while not self.request_queue.empty():
            try:
                _, _, item = self.request_queue.get_nowait()
                future = item['future']
                if not future.done():
                    future.set_exception(
                        HTTPException(status_code=503, detail=user_message)
                    )
                self.request_queue.task_done()
                drained += 1
            except asyncio.QueueEmpty:
                break
            except Exception as drain_err:
                logger.error(f"🔴 Error draining queue item: {drain_err}")
                break

        if drained:
            logger.warning(
                f"🔴 Drained {drained} queued request(s) due to DSP initialization failure"
            )

    async def _watchdog(self):
        """
        Background watchdog that force-releases stale DSP locks.

        Triggered when the active subprocess has produced NO output for
        TOKEN_SILENCE_TIMEOUT seconds — indicating it is unresponsive (e.g.
        the client disconnected mid-stream and the generator/subprocess are
        deadlocked on a full output buffer).

        A model that is legitimately generating a long response keeps calling
        update_inference_heartbeat() on every token, so the watchdog never
        fires for it regardless of total elapsed time.
        """
        logger.info(
            f"🐕 RequestQueueManager watchdog started "
            f"(TOKEN_SILENCE_TIMEOUT={TOKEN_SILENCE_TIMEOUT}s)"
        )
        while True:
            try:
                await asyncio.sleep(LOCK_WATCHDOG_INTERVAL)

                stale_session_id = None
                async with self.active_lock:
                    if self.active_session_id and self._last_token_at is not None:
                        silence = time.time() - self._last_token_at
                        if silence > TOKEN_SILENCE_TIMEOUT:
                            stale_session_id = self.active_session_id
                            logger.warning(
                                f"⏰ Watchdog: subprocess for session {stale_session_id} "
                                f"has been silent for {silence:.1f}s "
                                f"(>{TOKEN_SILENCE_TIMEOUT}s) — "
                                "force-releasing lock (unresponsive subprocess)"
                            )
                            self.active_session_id = None
                            self.active_event_id = None
                            self._last_token_at = None

                # Kill the subprocess and cancel the event outside the lock
                if stale_session_id:
                    try:
                        from openapi_server.managers.session_manager import SessionManager
                        session = SessionManager.get_instance().get_session(stale_session_id)
                        if session:
                            event = session.current_event
                            if event:
                                # Always force-kill the subprocess to free DSP resources,
                                # regardless of event state (ACTIVE, FAILED, etc.)
                                logger.info(
                                    f"⏰ Watchdog: force-killing subprocess for event "
                                    f"{event.event_id} (state={event.state.name})"
                                )
                                try:
                                    event.terminate_handle(force=True)
                                except Exception as kill_err:
                                    logger.error(f"⏰ Watchdog: error killing subprocess: {kill_err}")

                                # If still ACTIVE, do a full cancel (rolls back messages, etc.)
                                if event.is_active():
                                    logger.info(
                                        f"⏰ Watchdog: cancelling active event "
                                        f"{event.event_id} for session {stale_session_id}"
                                    )
                                    session.cancel_active_event()
                            else:
                                logger.info(
                                    f"⏰ Watchdog: no current event for session "
                                    f"{stale_session_id} — lock released, nothing to kill"
                                )
                    except Exception as cancel_err:
                        logger.error(f"⏰ Watchdog: error handling stale session {stale_session_id}: {cancel_err}")

            except asyncio.CancelledError:
                logger.info("Watchdog task cancelled - shutting down")
                break
            except Exception as e:
                logger.error(f"Watchdog error: {e}", exc_info=True)

    async def start_worker(self):
        """Start the background worker task."""
        if self.enabled and self.worker_task is None:
            self.worker_task = asyncio.create_task(self._worker())
            self.watchdog_task = asyncio.create_task(self._watchdog())
            logger.info("✓ RequestQueueManager worker task created")

    async def shutdown(self):
        """Graceful shutdown - drain queue and stop worker."""
        if self.worker_task:
            logger.info("Shutting down RequestQueueManager...")

            # Wait for queue to drain (with timeout)
            try:
                await asyncio.wait_for(
                    self.request_queue.join(),
                    timeout=30.0
                )
                logger.info("Queue drained successfully")
            except asyncio.TimeoutError:
                logger.warning("Queue drain timed out")

            # Cancel worker and watchdog
            self.worker_task.cancel()
            try:
                await self.worker_task
            except asyncio.CancelledError:
                pass

            if self.watchdog_task:
                self.watchdog_task.cancel()
                try:
                    await self.watchdog_task
                except asyncio.CancelledError:
                    pass

            logger.info("RequestQueueManager shutdown complete")

    def get_queue_size(self) -> int:
        """
        Get current queue size (for debugging).

        Returns:
            Number of requests in queue
        """
        return self.request_queue.qsize() if self.enabled else 0

    def get_active_session(self) -> Optional[str]:
        """
        Get currently active session ID (for debugging).

        Returns:
            Active session ID or None
        """
        return self.active_session_id

    def get_stats(self) -> Dict[str, Any]:
        """
        Get queue statistics (for debugging/monitoring).

        Returns:
            Dictionary with queue statistics
        """
        return {
            'enabled': self.enabled,
            'queue_size': self.get_queue_size(),
            'active_session_id': self.active_session_id,
            'active_event_id': self.active_event_id,
            'max_queue_size': MAX_QUEUE_SIZE,
            'request_timeout': REQUEST_TIMEOUT
        }

    async def cancel_request(self, session_id: str) -> bool:
        """
        Cancel a request for the given session.

        Handles two cases:
        1. Request is actively executing: release the DSP lock so the next request can proceed
        2. Request is queued: remove from queue and cancel the future

        Args:
            session_id: The session ID to cancel

        Returns:
            bool: True if a request was found and cancelled, False otherwise
        """
        if not self.enabled:
            return False

        logger.info(f"🚫 Attempting to cancel request for session {session_id}")

        # Case 1: Check if actively executing — release the lock
        async with self.active_lock:
            if self.active_session_id == session_id:
                logger.info(f"🔓 Session {session_id} is actively executing — releasing DSP lock")
                self.active_session_id = None
                self.active_event_id = None
                self._last_token_at = None
                return True

        # Case 2: Scan the queue and remove the matching entry
        temp_items = []
        found = False

        try:
            while not self.request_queue.empty():
                try:
                    item = self.request_queue.get_nowait()
                    priority, counter, queue_item = item

                    if queue_item['session'].session_id == session_id:
                        future = queue_item['future']
                        if not future.done():
                            future.set_exception(
                                Exception("Request cancelled by user")
                            )
                        logger.info(f"📤 Removed queued request for session {session_id}")
                        found = True
                        self.request_queue.task_done()
                    else:
                        temp_items.append(item)
                        self.request_queue.task_done()

                except asyncio.QueueEmpty:
                    break

            # Put back items we kept
            for item in temp_items:
                await self.request_queue.put(item)

            if found:
                logger.info(f"✅ Successfully cancelled queued request for session {session_id}")
            else:
                logger.warning(f"⚠️  No queued or active request found for session {session_id}")

            return found

        except Exception as e:
            logger.error(f"❌ Error cancelling request for session {session_id}: {e}", exc_info=True)
            for item in temp_items:
                try:
                    await self.request_queue.put(item)
                except Exception:
                    pass
            return False
