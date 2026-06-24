# Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
# SPDX-License-Identifier: BSD-3-Clause-Clear

from openapi_server.apis.chat_api_base import BaseChatApi
from openapi_server.models.create_chat_completion_request import CreateChatCompletionRequest
from openapi_server.models.create_chat_completion_response import CreateChatCompletionResponse
from openapi_server.models.chat_completion_deleted import ChatCompletionDeleted
from openapi_server.models.error import Error
from openapi_server.impl.event_based_chat_handler import EventBasedChatHandler
from openapi_server.logger.logger_config import LoggerConfig
from openapi_server.impl.constant import HttpStatusCodes, ErrorMessages
from openapi_server.managers.model_config_manager import ModelConfigManager
from openapi_server.managers.session_manager import SessionManager
from openapi_server.managers.request_queue_manager import RequestQueueManager

from fastapi import (
    HTTPException
)
from fastapi.responses import JSONResponse, StreamingResponse
from pydantic import StrictStr

LoggerConfig.initialize()
logger = LoggerConfig.get_logger(__name__)

class ChatApiImpl(BaseChatApi):
    async def create_chat_completion(
    self,
    create_chat_completion_request: CreateChatCompletionRequest,
    raw_json: dict = None
    ):
        """
        Creates a new chat conversation.
        Args:
            create_chat_completion_request (CreateChatCompletionRequest): The chat completion to create.
            raw_json (dict): Raw JSON request body to bypass Pydantic deserialization issues.
        Returns:
            StreamingResponse or JSONResponse: The created chat completion.
        Raises:
            HTTPException: If there is an error creating the chat completion.
        """
        try:
            # Validate the model if provided in the request
            if create_chat_completion_request.model:
                config_manager = ModelConfigManager()
                if not config_manager.validate_model(create_chat_completion_request.model):
                    error_message = ErrorMessages.MODEL_NOT_FOUND.format(model=create_chat_completion_request.model)
                    logger.error(f"Invalid model requested: {create_chat_completion_request.model}")
                    raise HTTPException(status_code=HttpStatusCodes.BAD_REQUEST, detail=error_message)
                logger.debug(f"Model validation passed for: {create_chat_completion_request.model}")

            # Map legacy max_tokens → max_completion_tokens for backward compatibility.
            # The OpenAI API deprecated max_tokens in favor of max_completion_tokens.
            # If max_completion_tokens is not set but max_tokens is, use max_tokens value
            # so that clients using the legacy field get correct budget calculations.
            if (
                getattr(create_chat_completion_request, 'max_completion_tokens', None) is None
                and getattr(create_chat_completion_request, 'max_tokens', None) is not None
            ):
                create_chat_completion_request.max_completion_tokens = (
                    create_chat_completion_request.max_tokens
                )
                logger.debug(
                    f"Mapped legacy max_tokens={create_chat_completion_request.max_tokens} "
                    "→ max_completion_tokens"
                )

            # FIX: Pass raw JSON to bypass Pydantic OneOf deserialization issues
            if raw_json:
                logger.debug("Received raw JSON dict to bypass Pydantic OneOf issues")
                logger.debug(f"Raw JSON contains {len(raw_json.get('messages', []))} messages")
            else:
                logger.debug("No raw JSON provided - may encounter Pydantic OneOf deserialization issues")

            # Resolve session at API layer (both ADHOC and normal modes)
            user_id = getattr(create_chat_completion_request, 'user', 'default_user')
            messages = EventBasedChatHandler.extract_messages_from_request(
                create_chat_completion_request, raw_json
            )

            session_manager = SessionManager.get_instance()
            session, is_new, replay_event = session_manager.find_or_create_session(
                user_id,
                messages,
                create_chat_completion_request,
                raw_json
            )

            if replay_event:
                logger.info(f"Resolved retry replay: session={session.session_id}, event={replay_event.event_id}")
                return EventBasedChatHandler._create_response(
                    replay_event.replay_result,
                    replay_event,
                    create_chat_completion_request,
                    session.session_id
                )

            logger.info(f"Resolved session: {session.session_id} (new={is_new})")

            # Route based on mode
            from openapi_server.impl.constant import ADHOC_MODE

            if ADHOC_MODE:
                # ADHOC_MODE: Use queue manager with pre-resolved session
                logger.info("Using RequestQueueManager for ADHOC_MODE")
                queue_manager = RequestQueueManager.get_instance()
                result = await queue_manager.enqueue_request(
                    create_chat_completion_request, raw_json, session
                )
            else:
                # Normal mode: Direct execution with pre-resolved session
                logger.info("Using EventBasedChatHandler directly (normal mode)")
                result = await EventBasedChatHandler.handle_chat_completion(
                    create_chat_completion_request, raw_json, session
                )

            if isinstance(result, Error):
                logger.error(f"Expected CreateChatCompletionResponse, got {type(result)}")
                raise HTTPException(status_code=int(result.code), detail=result.message)

            logger.info(f"create_chat_completion result: {result}")
            return result

        except HTTPException as http_exc:
            logger.error(f"HTTPException error in create_chat_completion: {http_exc}")
            from openapi_server.impl.constant import GenieErrorMappings

            if isinstance(http_exc.detail, dict) and "message" in http_exc.detail:
                return JSONResponse(
                    status_code=http_exc.status_code,
                    content={"error": http_exc.detail}
                )

            error_str = str(http_exc.detail)
            layman_msg = GenieErrorMappings.get_layman_message(error_str)
            final_msg = layman_msg if layman_msg else error_str

            return JSONResponse(
                status_code=http_exc.status_code,
                content={
                    "error": {
                        "message": final_msg,
                        "type": "server_error" if http_exc.status_code >= 500 else "invalid_request_error",
                        "param": None,
                        "code": http_exc.status_code
                    }
                }
            )

        except Exception as e:
            logger.error(f"Unexpected error in create_chat_completion: {e}")
            from openapi_server.impl.constant import GenieErrorMappings

            error_str = str(e)
            status_code = GenieErrorMappings.get_http_status_code(
                error_str,
                default_status=HttpStatusCodes.INTERNAL_SERVER_ERROR,
            )
            layman_msg = GenieErrorMappings.get_layman_message(error_str)
            final_msg = layman_msg if layman_msg else ErrorMessages.UNEXPECTED_ERROR

            return JSONResponse(
                status_code=status_code,
                content={
                    "error": {
                        "message": final_msg,
                        "type": "server_error",
                        "param": None,
                        "code": status_code
                    }
                }
            )

    async def delete_chat_completion(
        self,
        completion_id: StrictStr
    ) -> ChatCompletionDeleted:
        """
        Delete a stored chat completion and cleanup all associated resources.

        This will:
        - Terminate all event handles (current and historical)
        - Reset LLM singleton in ADHOC_MODE
        - Remove hash mappings
        - Remove tool calling mappings
        - Delete the session

        Args:
            completion_id: The ID of the chat completion to delete

        Returns:
            ChatCompletionDeleted: Deletion confirmation object

        Raises:
            HTTPException: If the chat completion is not found or deletion fails
        """
        try:
            logger.info(f"=== DELETE CHAT COMPLETION: {completion_id} ===")

            # Get session manager
            session_manager = SessionManager.get_instance()

            # Check if session exists
            session = session_manager.get_session(completion_id)
            if not session:
                logger.warning(f"Chat completion {completion_id} not found")
                raise HTTPException(
                    status_code=HttpStatusCodes.NOT_FOUND,
                    detail=f"Chat completion {completion_id} not found"
                )

            # Log session info before deletion
            logger.info(f"Deleting session {completion_id}: "
                       f"{len(session.events)} events, "
                       f"{len(session.messages)} messages, "
                       f"model={session.current_model}")

            # Delete session (includes all resource cleanup)
            success = session_manager.delete_session(completion_id)

            if success:
                logger.info(f"Successfully deleted chat completion {completion_id}")
                return ChatCompletionDeleted(
                    object="chat.completion.deleted",
                    id=completion_id,
                    deleted=True
                )
            else:
                # This shouldn't happen since we already checked existence
                logger.error(f"Failed to delete chat completion {completion_id}")
                raise HTTPException(
                    status_code=HttpStatusCodes.INTERNAL_SERVER_ERROR,
                    detail=f"Failed to delete chat completion {completion_id}"
                )

        except HTTPException:
            raise

        except Exception as e:
            logger.error(f"Unexpected error deleting chat completion {completion_id}: {e}", exc_info=True)
            raise HTTPException(
                status_code=HttpStatusCodes.INTERNAL_SERVER_ERROR,
                detail=f"Internal server error: {str(e)}"
            )

    async def cancel_chat_completion(
        self,
        completion_id: StrictStr
    ):
        """
        Cancel an actively executing chat completion.

        This will:
        - If in ADHOC_MODE and queued, remove from queue
        - If actively executing, force-kill subprocess and roll back session state
        - Preserve session history integrity

        Args:
            completion_id: The ID of the chat completion to cancel

        Returns:
            ChatCompletionCancelled: Cancellation confirmation object

        Raises:
            HTTPException: If the chat completion is not found or no active event exists
        """
        try:
            from openapi_server.models.chat_completion_cancelled import ChatCompletionCancelled

            logger.info(f"=== CANCEL CHAT COMPLETION: {completion_id} ===")

            session_manager = SessionManager.get_instance()
            session = session_manager.get_session(completion_id)

            if not session:
                logger.warning(f"Chat completion {completion_id} not found")
                raise HTTPException(
                    status_code=HttpStatusCodes.NOT_FOUND,
                    detail=f"Chat completion {completion_id} not found"
                )

            if not session.current_event or not session.current_event.is_active():
                logger.warning(f"No active event for chat completion {completion_id}")
                raise HTTPException(
                    status_code=HttpStatusCodes.BAD_REQUEST,
                    detail=f"No active event to cancel for chat completion {completion_id}"
                )

            logger.info(f"Cancelling active event {session.current_event.event_id} for session {completion_id}")

            # Step 1: Cancel in queue manager if ADHOC_MODE (removes from queue or releases lock)
            from openapi_server.impl.constant import ADHOC_MODE
            queue_cancelled = False
            if ADHOC_MODE:
                queue_manager = RequestQueueManager.get_instance()
                queue_cancelled = await queue_manager.cancel_request(completion_id)
                if queue_cancelled:
                    logger.info(f"Cancelled queued/active request for session {completion_id}")

            # Step 2: Cancel active event in session (force-kills subprocess, rolls back messages)
            session_cancelled = session.cancel_active_event()

            if session_cancelled or queue_cancelled:
                message = "Queued request cancelled" if queue_cancelled and not session_cancelled else \
                          "Active event cancelled and session state rolled back"
                logger.info(f"Successfully cancelled chat completion {completion_id}: {message}")
                return ChatCompletionCancelled(
                    object="chat.completion.cancelled",
                    id=completion_id,
                    cancelled=True,
                    message=message
                )
            else:
                logger.error(f"Failed to cancel chat completion {completion_id}")
                raise HTTPException(
                    status_code=HttpStatusCodes.INTERNAL_SERVER_ERROR,
                    detail=f"Failed to cancel chat completion {completion_id}"
                )

        except HTTPException:
            raise

        except Exception as e:
            logger.error(f"Unexpected error cancelling chat completion {completion_id}: {e}", exc_info=True)
            raise HTTPException(
                status_code=HttpStatusCodes.INTERNAL_SERVER_ERROR,
                detail=f"Internal server error: {str(e)}"
            )
