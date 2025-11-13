# Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
# SPDX-License-Identifier: BSD-3-Clause-Clear

from openapi_server.models.create_chat_completion_request import CreateChatCompletionRequest
from openapi_server.models.error import Error
from openapi_server.impl.genie_wrapper.gen_ai_service_singleton import LLMService
from openapi_server.logger.logger_config import LoggerConfig
from openapi_server.impl.genie_wrapper.utils.common_utils import CommonUtils
from openapi_server.impl.genie_wrapper.utils.handle_object_interface import HandleIdObjectMap, HandleObject
from openapi_server.impl.constant import HttpStatusCodes
from openapi_server.impl.constant import ErrorMessages, Parameters, LLMServiceKeys, LLMServiceQueryConstant as QUERY_CONST
from openapi_server.impl.genie_wrapper.chat.genie_wrapper_delete_chat_completion import GenieWrapperDeleteChatCompletion
from openapi_server.impl.model_config_manager import ModelConfigManager
from openapi_server.impl.conversation_tracker import ConversationMessage
from openapi_server.impl.genie_wrapper.summarization_service import SummarizationService
from openapi_server.impl.genie_wrapper.utils.token_counter import TokenCounter
import logging
import time
import hashlib
import uuid

LoggerConfig.initialize(level=logging.DEBUG)
logger = LoggerConfig.get_logger(__name__)

from fastapi import (
    HTTPException
)

class ChatQueryUtils:
    @staticmethod
    def _get_content_string(msg):
        """
        Helper method to extract content as string from a message object,
        handling text, objects, and image content types for proper hash calculation.

        For images: Uses URL if available, otherwise samples ~256 chars from base64.
        This ensures consistent hashing for session tracking with multimodal content.

        Args:
            msg: Message object
        Returns:
            String representation of content including image identifiers
        """
        if not hasattr(msg, "content"):
            return ""

        if msg.content is None:
            return ""

        if isinstance(msg.content, str):
            return msg.content.strip()

        # Handle list content (multimodal messages with text and images)
        if isinstance(msg.content, list):
            parts = []
            for item in msg.content:
                if isinstance(item, dict):
                    # Raw JSON content item
                    if item.get('type') == 'text':
                        text_content = item.get('text', '').strip()
                        if text_content:
                            parts.append(text_content)
                    elif item.get('type') == 'image_url':
                        image_url_data = item.get('image_url', {})
                        if isinstance(image_url_data, dict):
                            url = image_url_data.get('url', '')
                        else:
                            url = str(image_url_data)

                        if url:
                            if url.startswith('http'):
                                # Use full URL for hash (external images)
                                parts.append(f"[IMAGE:{url}]")
                            elif url.startswith('data:image'):
                                # Base64 image - sample ~256 characters for hash
                                # Format: data:image/jpeg;base64,<base64_data>
                                if ',' in url:
                                    base64_part = url.split(',', 1)[1]
                                    sample = base64_part[:256] if len(base64_part) > 256 else base64_part
                                    parts.append(f"[IMAGE:base64:{sample}]")
                                else:
                                    parts.append("[IMAGE:base64:invalid]")
                            else:
                                # Other image format
                                parts.append(f"[IMAGE:other:{url[:256]}]")

                # Handle Pydantic objects
                elif hasattr(item, 'type'):
                    if item.type == 'text' and hasattr(item, 'text'):
                        text_content = getattr(item, 'text', '').strip()
                        if text_content:
                            parts.append(text_content)
                    elif item.type == 'image_url' and hasattr(item, 'image_url'):
                        image_url_obj = getattr(item, 'image_url', None)
                        if image_url_obj:
                            if hasattr(image_url_obj, 'url'):
                                url = image_url_obj.url
                            elif isinstance(image_url_obj, dict):
                                url = image_url_obj.get('url', '')
                            else:
                                url = str(image_url_obj)

                            if url:
                                if url.startswith('http'):
                                    # Use full URL for hash (external images)
                                    parts.append(f"[IMAGE:{url}]")
                                elif url.startswith('data:image'):
                                    # Base64 image - sample ~256 characters for hash
                                    if ',' in url:
                                        base64_part = url.split(',', 1)[1]
                                        sample = base64_part[:256] if len(base64_part) > 256 else base64_part
                                        parts.append(f"[IMAGE:base64:{sample}]")
                                    else:
                                        parts.append("[IMAGE:base64:invalid]")
                                else:
                                    # Other image format
                                    parts.append(f"[IMAGE:other:{url[:256]}]")

            return " ".join(parts).strip()

        # Fallback: convert object to string representation
        return str(msg.content)

    @staticmethod
    def _has_tool_calls(msg):
        """
        Helper method to check if a message has tool calls.

        Args:
            msg: Message object
        Returns:
            Boolean indicating if message has tool calls
        """
        return hasattr(msg, "tool_calls") and msg.tool_calls and len(msg.tool_calls) > 0

    @staticmethod
    def _get_tool_call_info(msg):
        """
        Helper method to extract tool call information from a message.

        Args:
            msg: Message object with tool calls
        Returns:
            String representation of tool call information
        """
        if not ChatQueryUtils._has_tool_calls(msg):
            return ""

        tool_info = []
        for tc in msg.tool_calls:
            if hasattr(tc, "function") and hasattr(tc, "id"):
                tool_info.append(f"{tc.function.name}:{tc.id}")

        return "|".join(tool_info)

    @staticmethod
    def calculate_conversation_hash(messages, exclude_last_pair=True):
        """
        Calculate deterministic hash of complete user-assistant pairs only,
        including tool calling sequences.

        This ensures consistent hashing across requests as the conversation grows.
        System messages are excluded from hash calculation as they are context, not conversation.

        Tool calling sequences (user → assistant(tool_call) → tool → assistant(response))
        are treated as a single complete pair. Incomplete tool sequences are handled specially.

        Args:
            messages: List of message objects
            exclude_last_pair: If True, exclude the last complete pair
        Returns:
            16-character hex hash string, or empty string if no complete pairs to hash
        """
        # Filter out system messages before calculating hash
        conversation_messages = [msg for msg in messages if msg.role != "system"]

        complete_pairs = []
        incomplete_sequence = None
        i = 0

        while i < len(conversation_messages):
            if conversation_messages[i].role == "user":
                user_msg = conversation_messages[i]
                i += 1

                # Collect ALL assistant and tool messages until next user message
                assistant_sequence = []
                while i < len(conversation_messages) and conversation_messages[i].role != "user":
                    assistant_sequence.append(conversation_messages[i])
                    i += 1

                # If we have assistant messages, check if this is a complete pair
                if assistant_sequence:
                    # Find the FINAL assistant message (last one with content)
                    final_assistant = None
                    for msg in reversed(assistant_sequence):
                        if msg.role == "assistant" and ChatQueryUtils._get_content_string(msg):
                            final_assistant = msg
                            break

                    if final_assistant:
                        # This is a COMPLETE pair - has final assistant response
                        tool_calls_info = []
                        for msg in assistant_sequence:
                            if msg.role == "assistant" and ChatQueryUtils._has_tool_calls(msg):
                                tool_info = ChatQueryUtils._get_tool_call_info(msg)
                                if tool_info:
                                    tool_calls_info.append(tool_info)

                        complete_pairs.append({
                            'user': user_msg,
                            'assistant': final_assistant,
                            'tool_calls': tool_calls_info
                        })
                    else:
                        # This is an INCOMPLETE sequence - no final assistant response yet
                        # Check if it has tool calls (indicating it's a tool calling sequence)
                        has_tool_call = any(
                            msg.role == "assistant" and ChatQueryUtils._has_tool_calls(msg)
                            for msg in assistant_sequence
                        )

                        if has_tool_call:
                            # Store as incomplete sequence for special handling
                            incomplete_sequence = {
                                'user': user_msg,
                                'sequence': assistant_sequence
                            }
                            logger.debug(f"Found incomplete tool calling sequence for user: {ChatQueryUtils._get_content_string(user_msg)[:50]}...")
                        # If no tool calls, it's just a malformed sequence - skip it
                else:
                    # User message with no assistant response - this could be the current query
                    # Don't treat as incomplete sequence, just ignore for now
                    pass
            else:
                # Skip orphaned assistant/tool messages (shouldn't happen)
                i += 1

        # Apply exclude_last_pair logic with special handling for incomplete sequences
        if exclude_last_pair:
            if incomplete_sequence:
                # If we have an incomplete sequence at the end, exclude it entirely
                # Hash only the complete pairs before it
                pairs_to_hash = complete_pairs
                logger.debug(f"Excluding incomplete tool sequence, hashing {len(pairs_to_hash)} complete pairs")
            elif len(complete_pairs) > 0:
                # Normal case - exclude the last complete pair
                pairs_to_hash = complete_pairs[:-1]
                logger.debug(f"Excluding last complete pair, hashing {len(pairs_to_hash)} pairs")
            else:
                pairs_to_hash = []
        else:
            # Include all complete pairs, but NOT incomplete sequences
            pairs_to_hash = complete_pairs
            if incomplete_sequence:
                logger.debug(f"Not including incomplete sequence in hash, using {len(pairs_to_hash)} complete pairs")

        if not pairs_to_hash:
            logger.debug("No complete pairs to hash, returning empty string")
            return ""

        # Build hash string
        conversation_str = ""
        for pair in pairs_to_hash:
            user_content = ChatQueryUtils._get_content_string(pair['user'])
            asst_content = ChatQueryUtils._get_content_string(pair['assistant'])

            # Include tool call info for determinism
            if pair['tool_calls']:
                tool_info = "|".join(pair['tool_calls'])
                conversation_str += f"user:{user_content}|tools:{tool_info}|assistant:{asst_content}|"
            else:
                conversation_str += f"user:{user_content}|assistant:{asst_content}|"

        hash_result = hashlib.sha256(conversation_str.encode()).hexdigest()[:16]
        logger.debug(f"Calculated hash for {len(pairs_to_hash)} complete pairs: {hash_result}")
        return hash_result

    @staticmethod
    def find_thread_by_hash(safety_identifier, conversation_hash, map_obj):
        """
        Find existing thread by matching conversation hash.
        Args:
            safety_identifier: User identifier
            conversation_hash: Hash to match against
            map_obj: HandleIdObjectMap instance
        Returns:
            (composite_key, handle_obj) or (None, None)
        """
        user_threads = map_obj.get_user_threads(safety_identifier)

        for key, handle_obj in user_threads:
            if handle_obj.conversation_hash == conversation_hash:
                logger.info(f"Found matching thread: {key} with hash {conversation_hash}")
                return key, handle_obj

        logger.debug(f"No matching thread found for user {safety_identifier} with hash {conversation_hash}")
        return None, None

    @staticmethod
    def chat_compose_query(request_data: CreateChatCompletionRequest, completion_id: str = None):
        """
        Executes a chat query using the provided request data with dynamic model selection.

        Args:
            request_data (CreateChatCompletionRequest): The request data containing the chat query.
            completion_id (str, optional): The completion ID. Defaults to None.

        Returns:
            tuple: A tuple containing the error (if any), the handle, the query, and completion_id.

        """
        if not request_data.messages:
            raise ValueError("No messages provided in request")

        # Separate system messages from conversation messages
        system_messages = [msg for msg in request_data.messages if msg.role == "system"]
        conversation_messages = [msg for msg in request_data.messages if msg.role != "system"]

        # Concatenate all system messages
        system_context = "\n\n".join([msg.content for msg in system_messages]) if system_messages else ""

        # Get user identifier for tracking
        safety_identifier = request_data.user or "anonymous"

        if system_context:
            logger.info(f"Extracted {len(system_messages)} system message(s) with total length {len(system_context)} chars")
            # Store system prompt in conversation tracker for later use during summarization
            from openapi_server.impl.conversation_tracker import ConversationTracker
            tracker = ConversationTracker()
            tracker.update_system_prompt(safety_identifier, system_context)
            logger.info(f"Stored system prompt for user {safety_identifier}: {len(system_context)} chars")

        # Ensure at least one conversation message exists
        if not conversation_messages:
            raise HTTPException(
                status_code=HttpStatusCodes.BAD_REQUEST,
                detail="At least one user or assistant message is required"
            )

        # Validate message alternation (user/assistant pattern) - ONLY for conversation messages
        if len(conversation_messages) > 1:
            for i in range(len(conversation_messages) - 1):
                current_role = conversation_messages[i].role
                next_role = conversation_messages[i + 1].role

                # Check for consecutive messages with same role
                if current_role == next_role:
                    error_msg = f"Invalid message sequence: Found consecutive '{current_role}' messages at positions {i} and {i+1}. Messages must alternate between 'user' and 'assistant' roles."
                    logger.error(error_msg)
                    raise HTTPException(
                        status_code=HttpStatusCodes.BAD_REQUEST,
                        detail=error_msg
                    )

                # Ensure proper alternation (user -> assistant -> user -> assistant)
                if i == 0 and current_role != "user":
                    error_msg = f"Invalid message sequence: First conversation message must be from 'user', but found '{current_role}'."
                    logger.error(error_msg)
                    raise HTTPException(
                        status_code=HttpStatusCodes.BAD_REQUEST,
                        detail=error_msg
                    )

            logger.debug(f"Message alternation validation passed for {len(conversation_messages)} conversation messages")

        # Get the last conversation message
        last_msg = conversation_messages[-1]

        # Check if content is empty or whitespace, handling both string and object content
        content_is_empty = False
        if not last_msg.content:
            content_is_empty = True
        elif isinstance(last_msg.content, str) and not last_msg.content.strip():
            content_is_empty = True

        if content_is_empty:
            raise HTTPException(status_code=HttpStatusCodes.BAD_REQUEST, detail=ErrorMessages.INCORRECT_CONTENT)

        # Note: System messages are kept separate in request_data.messages
        # They will be formatted properly by the backend with the template
        if system_context:
            logger.info(f"System context present: {len(system_context)} chars (will be sent as separate system role)")

        # Get model from request or fall back to default from config
        requested_model = getattr(request_data, 'model', None)

        # Check if model is provided and not empty
        if requested_model and requested_model.strip():
            logger.info(f"Model requested from API: {requested_model}")
            # Use external model ID directly (no internal ID mapping needed)
            model_str = requested_model
        else:
            # Fall back to default model from config (returns external ID)
            config_manager = ModelConfigManager()
            model_str = config_manager.get_default_model()
            logger.info(f"No model in request, using default: {model_str}")

        logger.info(f"Using model: {model_str}")

        # Check if automatic summarization is needed based on context window
        config_manager = ModelConfigManager()
        context_window = config_manager.get_context_size(model_str)
        threshold_percentage = 0.7  # 70% threshold
        threshold_tokens = int(context_window * threshold_percentage)

        # Calculate total tokens in conversation
        # For continuing conversations, check if we have summarization tracking
        map_obj = HandleIdObjectMap()
        existing_handle_obj = None

        if len(request_data.messages) > 1:
            # Try to find existing handle to check summarization status
            lookup_hash = ChatQueryUtils.calculate_conversation_hash(
                request_data.messages,
                exclude_last_pair=True
            )
            safety_identifier = request_data.user or "anonymous"
            _, existing_handle_obj = ChatQueryUtils.find_thread_by_hash(
                safety_identifier,
                lookup_hash,
                map_obj
            )

        # Calculate tokens based on summarization status
        if existing_handle_obj and existing_handle_obj.last_summarization_index >= 0:
            # Summarization has occurred - only count summary + messages after summarization
            messages_after_summary = request_data.messages[existing_handle_obj.last_summarization_index + 1:]
            tokens_after_summary = sum(TokenCounter.estimate_tokens(msg.content) for msg in messages_after_summary)
            total_tokens = existing_handle_obj.summary_token_count + tokens_after_summary
            logger.info(f"Token count with summarization: {existing_handle_obj.summary_token_count} (summary) + {tokens_after_summary} (after index {existing_handle_obj.last_summarization_index}) = {total_tokens}")
        else:
            # No summarization yet - count all messages
            total_tokens = sum(TokenCounter.estimate_tokens(msg.content) for msg in request_data.messages)
            logger.info(f"Token count without summarization: {total_tokens} (all messages)")

        logger.info(f"Conversation tokens: {total_tokens}/{context_window} (threshold: {threshold_tokens})")

        # Trigger automatic summarization if threshold exceeded
        if total_tokens >= threshold_tokens and len(request_data.messages) > 1:
            logger.info(f"Context threshold exceeded ({total_tokens} >= {threshold_tokens}), triggering summarization")

            # Extract user-assistant pairs (excluding the latest user message)
            messages_without_current = request_data.messages[:-1]

            # Try to get last 2 pairs (4 messages: user, assistant, user, assistant)
            if len(messages_without_current) >= 4:
                last_two_pairs = messages_without_current[-4:]
                two_pairs_tokens = sum(TokenCounter.estimate_tokens(msg.content) for msg in last_two_pairs)

                if two_pairs_tokens < threshold_tokens:
                    messages_to_summarize = last_two_pairs
                    logger.info(f"Summarizing last 2 pairs ({two_pairs_tokens} tokens)")
                else:
                    # Fall back to 1 pair (2 messages: user, assistant)
                    last_one_pair = messages_without_current[-2:]
                    messages_to_summarize = last_one_pair
                    one_pair_tokens = sum(TokenCounter.estimate_tokens(msg.content) for msg in last_one_pair)
                    logger.info(f"2 pairs too large, summarizing last 1 pair ({one_pair_tokens} tokens)")
            elif len(messages_without_current) >= 2:
                # Only 1 pair available
                messages_to_summarize = messages_without_current[-2:]
                pair_tokens = sum(TokenCounter.estimate_tokens(msg.content) for msg in messages_to_summarize)
                logger.info(f"Summarizing last available pair ({pair_tokens} tokens)")
            else:
                # Not enough messages to summarize
                messages_to_summarize = []
                logger.warning("Not enough message pairs to summarize")

            if messages_to_summarize:
                # Convert to ConversationMessage format, filtering out tool infrastructure
                conv_messages = []
                for msg in messages_to_summarize:
                    # Skip assistant messages that only have tool calls (no content)
                    if msg.role == "assistant":
                        has_tool_calls = hasattr(msg, 'tool_calls') and msg.tool_calls and len(msg.tool_calls) > 0
                        has_content = msg.content and (isinstance(msg.content, str) and msg.content.strip())

                        if has_tool_calls and not has_content:
                            logger.debug(f"Skipping assistant tool call message from summarization")
                            continue

                    # Skip tool response messages
                    if msg.role == "tool":
                        logger.debug(f"Skipping tool response message from summarization")
                        continue

                    # Include this message in summarization
                    conv_msg = ConversationMessage(
                        role=msg.role,
                        content=msg.content,
                        tokens=TokenCounter.estimate_tokens(msg.content)
                    )
                    conv_messages.append(conv_msg)

                logger.info(f"Filtered {len(messages_to_summarize)} messages to {len(conv_messages)} for summarization")

                # Perform summarization with retry logic
                summary_text = None
                llm_service = LLMService()

                # Get existing handle if available for reset (only for continuing conversations)
                existing_handle = None
                if len(request_data.messages) > 1:  # Not a new conversation
                    map_obj = HandleIdObjectMap()
                    if map_obj.get_current_size() > 0:
                        conv_ids = map_obj.get_all_conversation()
                        if conv_ids and len(conv_ids) > 0:
                            handle_obj = map_obj.get_handle(conv_ids[0])
                            if handle_obj:
                                existing_handle = handle_obj.handle_object

                # Reset dialog before summarization
                if existing_handle:
                    logger.info("Resetting dialog before summarization")
                    llm_service.lib.llm_reset_object(existing_handle)
                    time.sleep(1)  # Wait for reset to complete
                    logger.info("Reset complete, proceeding with summarization")

                # Log messages being summarized
                logger.info(f"=== SUMMARIZATION INPUT: {len(conv_messages)} messages ===")
                for i, msg in enumerate(conv_messages):
                    logger.info(f"  Message {i+1} [{msg.role}]: {len(msg.content)} chars, ~{msg.tokens} tokens")
                    logger.debug(f"  Content preview: {msg.content[:200]}...")

                # Try summarization with retry
                for attempt in range(2):
                    try:
                        logger.info(f"Summarization attempt {attempt + 1}")
                        summary_text, summary_tokens = SummarizationService.summarize_conversation(
                            messages=conv_messages,
                            model_str=model_str,
                            llm_handle=existing_handle,
                            max_summary_tokens=400
                        )
                        logger.info(f"Summarization successful: {summary_tokens} tokens")
                        time.sleep(1)  # Wait for summarization to fully complete
                        logger.info("Summarization complete, proceeding with reset")
                        break
                    except Exception as e:
                        logger.error(f"Summarization attempt {attempt + 1} failed: {e}")
                        if attempt == 1:
                            raise RuntimeError(f"Summarization failed after 2 attempts: {e}")

                # Reset dialog after summarization
                if existing_handle:
                    logger.info("Resetting dialog after summarization")
                    llm_service.lib.llm_reset_object(existing_handle)
                    time.sleep(1)  # Wait for reset to complete
                    logger.info("Final reset complete")
                    time.sleep(1)  # User requested second wait after final reset

                # Prepend summary to current query and update tracking
                if summary_text and existing_handle_obj:
                    last_msg = request_data.messages[-1]
                    enhanced_content = f"[Previous conversation summary: {summary_text}]\n\n{last_msg.content}"
                    logger.info(f"Enhanced query with summary. Original: {len(last_msg.content)} chars, Enhanced: {len(enhanced_content)} chars")
                    last_msg.content = enhanced_content

                    # Update summarization tracking in handle object
                    # The last_summarization_index should point to the last message that was summarized
                    # Since we summarized messages_to_summarize, we need to find their position in the full message list
                    summarized_count = len(messages_to_summarize)
                    existing_handle_obj.last_summarization_index = len(request_data.messages) - 1 - summarized_count
                    existing_handle_obj.summary_token_count = summary_tokens

                    logger.info(f"Updated summarization tracking: last_index={existing_handle_obj.last_summarization_index}, summary_tokens={summary_tokens}")
                    logger.info(f"Messages after summarization will start from index {existing_handle_obj.last_summarization_index + 1}")
            else:
                # No messages to summarize - check if it's because conversation is too large
                if len(messages_without_current) >= 2:
                    # We have pairs but they're too large to summarize
                    logger.warning("Conversation pairs too large to summarize, resetting dialog and using only latest query")

                    # Get existing handle for reset
                    if len(request_data.messages) > 1:
                        llm_service = LLMService()
                        map_obj = HandleIdObjectMap()
                        if map_obj.get_current_size() > 0:
                            conv_ids = map_obj.get_all_conversation()
                            if conv_ids and len(conv_ids) > 0:
                                handle_obj = map_obj.get_handle(conv_ids[0])
                                if handle_obj and handle_obj.handle_object:
                                    logger.info("Resetting dialog due to oversized conversation")
                                    llm_service.lib.llm_reset_object(handle_obj.handle_object)

        # Thread-based session management
        llm_service = LLMService()
        handle = None
        query = None
        map_obj = HandleIdObjectMap()

        # Get user identifier
        safety_identifier = request_data.user or "anonymous"
        logger.info(f"Processing request for user: {safety_identifier}, total messages: {len(request_data.messages)}, conversation messages: {len(conversation_messages)}")

        # Check if this is a continuing conversation by looking for existing thread
        # Calculate hash to see if we have an existing conversation
        is_new_conversation = len(conversation_messages) == 1
        existing_thread_key = None
        existing_handle_obj = None

        if not is_new_conversation:
            # Try to find existing thread for continuing conversation
            lookup_hash = ChatQueryUtils.calculate_conversation_hash(
                request_data.messages,
                exclude_last_pair=True
            )
            logger.info(f"Looking up existing thread with hash: {lookup_hash}")
            existing_thread_key, existing_handle_obj = ChatQueryUtils.find_thread_by_hash(
                safety_identifier,
                lookup_hash,
                map_obj
            )

            if not existing_handle_obj:
                # No matching thread found, but we have multiple messages
                # This shouldn't happen in normal flow, but treat as error
                logger.warning(f"Multiple conversation messages but no matching thread found")

        # NEW CONVERSATION (only 1 conversation message AND no existing thread)
        if is_new_conversation:
            logger.info(f"=== NEW CONVERSATION for user {safety_identifier} ===")

            # Get next thread number for this user
            thread_number = map_obj.get_next_thread_number(safety_identifier)
            composite_key = f"{safety_identifier}:thread_{thread_number}"

            logger.info(f"Creating new thread {thread_number} with composite key: {composite_key}")

            # Create new LLM handle
            config_path = CommonUtils.get_model_config_path(model_str)
            model_input = llm_service.ffi.new("char[]", model_str.encode('utf-8'))
            if model_input == llm_service.ffi.NULL:
                logger.error("Failed to allocate Model pointer")
                err = Error(code=f"{HttpStatusCodes.INTERNAL_SERVER_ERROR}",
                           message=ErrorMessages.MEM_ALLOCATION_ERR,
                           param=Parameters.INTERNAL_TYPE,
                           type=Parameters.INTERNAL_TYPE)
                return err, None, None, None

            config_path_input = llm_service.ffi.new("char[]", config_path.encode('utf-8'))
            if config_path_input == llm_service.ffi.NULL:
                logger.error("Failed to allocate config path pointer")
                err = Error(code=f"{HttpStatusCodes.INTERNAL_SERVER_ERROR}",
                           message=ErrorMessages.MEM_ALLOCATION_ERR,
                           param=Parameters.INTERNAL_TYPE,
                           type=Parameters.INTERNAL_TYPE)
                return err, None, None, None

            try:
                handle = llm_service.lib.llm_create_object(model_input, config_path_input, request_data.stream)
                if handle == llm_service.ffi.NULL:
                    logger.error("Failed to create LLM object: received NULL pointer")
                    err = Error(code=f"{HttpStatusCodes.INTERNAL_SERVER_ERROR}",
                               message=ErrorMessages.OBJ_CREATION_FAILED,
                               param=Parameters.LLM_OBJECT,
                               type=Parameters.INTERNAL_TYPE)
                    return err, None, None, None
            except Exception as e:
                logger.error(f"Exception during model initialization for {model_str}: {str(e)}")
                err = Error(code=f"{HttpStatusCodes.SERVICE_UNAVAILABLE}",
                           message=ErrorMessages.MODEL_INIT_FAILED.format(model=model_str),
                           param=Parameters.LLM_OBJECT,
                           type=Parameters.INTERNAL_TYPE)
                return err, None, None, None

            # Generate OpenAI-style UUID for completion_id
            openai_uuid = f"chatcmpl-{uuid.uuid4().hex[:24]}"
            # Create HandleObject with thread information
            handle_obj = HandleObject(
                handle,
                msg_count=1,
                model_id=model_str,
                safety_identifier=safety_identifier,
                thread_number=thread_number
            )
            handle_obj.conversation_hash = ""  # Empty for first message
            handle_obj.completion_id = openai_uuid

            # Store with composite key, but use UUID in API
            map_obj.set_handle(handle_obj, composite_key)

            logger.info(f"New thread created successfully: {composite_key} with completion_id: {openai_uuid}")
            completion_id = openai_uuid

        # CONTINUING CONVERSATION (multiple conversation messages)
        else:
            logger.info(f"=== CONTINUING CONVERSATION for user {safety_identifier} ===")

            # Use the thread we already found
            composite_key = existing_thread_key
            handle_obj = existing_handle_obj

            if not handle_obj:
                error_msg = f"No matching conversation thread found for user {safety_identifier}"
                logger.error(error_msg)
                logger.error(f"Available threads for user: {[k for k, _ in map_obj.get_user_threads(safety_identifier)]}")
                err = Error(code=f"{HttpStatusCodes.NOT_FOUND}",
                           message=error_msg,
                           param=Parameters.COMPLETION_ID,
                           type=Parameters.INTERNAL_TYPE)
                return err, None, None, None

            logger.info(f"Found existing thread: {composite_key} (thread #{handle_obj.thread_number})")

            # Calculate new hash but don't update yet - will update after model switch if needed
            new_hash = ChatQueryUtils.calculate_conversation_hash(
                request_data.messages,
                exclude_last_pair=False
            )

            # Check message pair limit
            if handle_obj.message_pairs_count >= QUERY_CONST.MAX_MESSAGE_PAIRS:
                err = Error(code=f"{HttpStatusCodes.BAD_REQUEST}",
                           message=ErrorMessages.MAX_MSG_LIMIT_REACHED,
                           param=Parameters.COMPLETION_ID,
                           type=Parameters.INTERNAL_TYPE)
                return err, None, None, None

            # Check for model switch
            if handle_obj.model_id and handle_obj.model_id != model_str:
                logger.info(f"Model switch detected for user {safety_identifier}: {handle_obj.model_id} → {model_str}")

                # Store old model info
                old_model = handle_obj.model_id

                # Validate the new model first
                config_manager = ModelConfigManager()
                if not config_manager.validate_model(requested_model):  # Use original requested_model, not internal ID
                    error_message = ErrorMessages.MODEL_NOT_FOUND.format(model=requested_model)
                    logger.error(f"Invalid model requested for switch: {requested_model}")
                    err = Error(code=f"{HttpStatusCodes.BAD_REQUEST}",
                               message=error_message,
                               param="model",
                               type="invalid_request_error")
                    return err, None, None, None

                logger.info(f"Model validation passed for switch to: {requested_model}")

                try:
                    # Check if we already have a handle for this model for this user
                    existing_model_handle = map_obj.find_handle_by_user_and_model(safety_identifier, model_str)

                    if existing_model_handle:
                        # Scenario 1: Use existing handle for this model
                        logger.info(f"Found existing handle for model {model_str}, reusing it")
                        existing_key, existing_handle_obj = existing_model_handle

                        # Reset the existing handle before reuse
                        llm_service.lib.llm_reset_object(existing_handle_obj.handle_object)
                        time.sleep(1)  # Wait for reset to complete

                        # Use the existing handle
                        handle = existing_handle_obj.handle_object
                        completion_id = existing_handle_obj.completion_id

                        # Update handle object with model switch info
                        existing_handle_obj.model_switch_count += 1
                        existing_handle_obj.previous_model_id = old_model

                        # Update the composite key to use
                        composite_key = existing_key
                        handle_obj = existing_handle_obj
                    else:
                        # Scenario 2: Create new handle for this model
                        logger.info(f"Creating new handle for model {model_str}")

                        # Create new LLM handle for the new model
                        config_path = CommonUtils.get_model_config_path(model_str)
                        model_input = llm_service.ffi.new("char[]", model_str.encode('utf-8'))
                        if model_input == llm_service.ffi.NULL:
                            logger.error("Failed to allocate Model pointer for model switch")
                            err = Error(code=f"{HttpStatusCodes.INTERNAL_SERVER_ERROR}",
                                      message=ErrorMessages.MODEL_SWITCH_ERROR.format(
                                          old_model=old_model,
                                          new_model=model_str,
                                          error=ErrorMessages.MEM_ALLOCATION_ERR),
                                      param=Parameters.INTERNAL_TYPE,
                                      type=Parameters.INTERNAL_TYPE)
                            return err, None, None, None

                        config_path_input = llm_service.ffi.new("char[]", config_path.encode('utf-8'))
                        if config_path_input == llm_service.ffi.NULL:
                            logger.error("Failed to allocate config path pointer for model switch")
                            err = Error(code=f"{HttpStatusCodes.INTERNAL_SERVER_ERROR}",
                                      message=ErrorMessages.MODEL_SWITCH_ERROR.format(
                                          old_model=old_model,
                                          new_model=model_str,
                                          error=ErrorMessages.MEM_ALLOCATION_ERR),
                                      param=Parameters.INTERNAL_TYPE,
                                      type=Parameters.INTERNAL_TYPE)
                            return err, None, None, None

                        try:
                            new_handle = llm_service.lib.llm_create_object(model_input, config_path_input, request_data.stream)
                            if new_handle == llm_service.ffi.NULL:
                                logger.error("Failed to create LLM object for model switch: received NULL pointer")
                                err = Error(code=f"{HttpStatusCodes.INTERNAL_SERVER_ERROR}",
                                          message=ErrorMessages.MODEL_SWITCH_ERROR.format(
                                              old_model=old_model,
                                              new_model=model_str,
                                              error=ErrorMessages.OBJ_CREATION_FAILED),
                                          param=Parameters.LLM_OBJECT,
                                          type=Parameters.INTERNAL_TYPE)
                                return err, None, None, None
                        except Exception as e:
                            logger.error(f"Exception during model initialization for model switch from {old_model} to {model_str}: {str(e)}")
                            err = Error(code=f"{HttpStatusCodes.SERVICE_UNAVAILABLE}",
                                      message=ErrorMessages.MODEL_SWITCH_ERROR.format(
                                          old_model=old_model,
                                          new_model=model_str,
                                          error=ErrorMessages.MODEL_INIT_FAILED.format(model=model_str)),
                                      param=Parameters.LLM_OBJECT,
                                      type=Parameters.INTERNAL_TYPE)
                            return err, None, None, None

                        # Create a new thread number for this model
                        thread_number = map_obj.get_next_thread_number(safety_identifier)
                        new_composite_key = f"{safety_identifier}:thread_{thread_number}"

                        # Generate new OpenAI-style UUID for completion_id
                        new_openai_uuid = f"chatcmpl-{uuid.uuid4().hex[:24]}"

                        # Create new HandleObject with thread information
                        new_handle_obj = HandleObject(
                            new_handle,
                            msg_count=1,
                            model_id=model_str,
                            safety_identifier=safety_identifier,
                            thread_number=thread_number
                        )
                        new_handle_obj.conversation_hash = handle_obj.conversation_hash  # Preserve conversation tracking
                        logger.info(f"Preserved conversation hash during model switch: {handle_obj.conversation_hash}")
                        new_handle_obj.completion_id = new_openai_uuid
                        new_handle_obj.model_switch_count = 1
                        new_handle_obj.previous_model_id = old_model

                        # Store with composite key, but use UUID in API
                        map_obj.set_handle(new_handle_obj, new_composite_key)

                        # Update references
                        handle = new_handle
                        completion_id = new_openai_uuid
                        composite_key = new_composite_key
                        handle_obj = new_handle_obj

                        logger.info(f"New handle created for model switch: {composite_key} with completion_id: {completion_id}")

                    # Prepare context for model switch
                    # Extract last 2 conversation turns (if available) and current query
                    context_messages = []

                    # Get system prompt if available
                    from openapi_server.impl.conversation_tracker import ConversationTracker
                    tracker = ConversationTracker()
                    system_prompt = tracker.get_system_prompt(safety_identifier)

                    # Extract last assistant message and last user message (if available)
                    if len(conversation_messages) >= 3:
                        # Get last assistant message (second-to-last in conversation)
                        last_assistant_msg = conversation_messages[-2]
                        if last_assistant_msg.role == "assistant":
                            context_messages.append(last_assistant_msg)

                        # Get last user message (third-to-last in conversation)
                        last_user_msg = conversation_messages[-3]
                        if last_user_msg.role == "user":
                            context_messages.append(last_user_msg)

                    # Add current user message
                    context_messages.append(last_msg)

                    # Calculate token count for the context-aware prompt
                    context_prompt_tokens = 0

                    # Add system prompt tokens (if present)
                    if system_prompt:
                        context_prompt_tokens += TokenCounter.estimate_tokens(system_prompt)

                    # Add context message tokens
                    for msg in context_messages:
                        context_prompt_tokens += TokenCounter.estimate_tokens(msg.content)

                    # Apply 1.3 multiplier for formatting/overhead
                    total_tokens = int(context_prompt_tokens * 1.3)

                    # Reset token tracking for the handle
                    handle_obj.total_conversation_tokens = total_tokens
                    handle_obj.summary_token_count = 0
                    handle_obj.last_summarization_index = -1

                    logger.info(f"Model switch complete. New token count: {total_tokens}")

                except Exception as e:
                    logger.error(f"Error during model switch: {e}")
                    err = Error(code=f"{HttpStatusCodes.INTERNAL_SERVER_ERROR}",
                               message=ErrorMessages.MODEL_SWITCH_ERROR.format(
                                   old_model=old_model,
                                   new_model=model_str,
                                   error=str(e)),
                               param=Parameters.INTERNAL_TYPE,
                               type=Parameters.INTERNAL_TYPE)
                    return err, None, None, None

            # Now it's safe to update the hash - either no model switch was needed or it succeeded
            handle_obj.conversation_hash = new_hash
            logger.info(f"Updated thread hash from {lookup_hash} to {new_hash}")

            # Reuse existing handle
            handle = handle_obj.handle_object
            completion_id = handle_obj.completion_id

            logger.info(f"Reusing handle for thread {composite_key} with completion_id: {completion_id}")

        # Create query structure
        query = llm_service.ffi.new(LLMServiceKeys.QUERY)
        if query == llm_service.ffi.NULL:
            logger.error("Failed to allocate Query pointer.")
            err = Error(code=f"{HttpStatusCodes.INTERNAL_SERVER_ERROR}",
                       message=ErrorMessages.MEM_ALLOCATION_ERR,
                       param=Parameters.INTERNAL_TYPE,
                       type=Parameters.INTERNAL_TYPE)
            return err, None, None, None

        # Populate query fields
        CommonUtils.copy_py_string_to_c_array(llm_service.ffi, query.model,
                        model_str, QUERY_CONST.MODEL_STR_MAX_SIZE)

        # Build the complete formatted prompt with system + user messages
        formatted_parts = []

        # Add system messages if present
        if system_context:
            formatted_system = CommonUtils.format_message_with_template(
                model_str, "system", system_context
            )
            formatted_parts.append(formatted_system)
            logger.info(f"Formatted system message: {len(formatted_system)} chars")

        # Add user message
        formatted_user = CommonUtils.format_message_with_template(
            model_str, last_msg.role, last_msg.content
        )
        formatted_parts.append(formatted_user)
        logger.info(f"Formatted user message: {len(formatted_user)} chars")

        # Add assistant prompt at the end
        assistant_prompt = CommonUtils.get_assistant_prompt(model_str)
        if assistant_prompt:
            formatted_parts.append(assistant_prompt)
            logger.info(f"Added assistant prompt: {len(assistant_prompt)} chars")

        # Combine all parts
        formatted_content = "".join(formatted_parts)
        logger.info(f"Total formatted content: {len(formatted_content)} chars")

        CommonUtils.copy_py_string_to_c_array(llm_service.ffi, query.message.role,
                        last_msg.role, QUERY_CONST.ROLE_MAX_SIZE)
        CommonUtils.copy_py_string_to_c_array(llm_service.ffi, query.message.content,
                        formatted_content, QUERY_CONST.MESSAGE_CONTENT_MAX_SIZE)

        # Set numeric parameters - use 300 as default if not provided
        max_tokens = request_data.max_completion_tokens if request_data.max_completion_tokens else QUERY_CONST.DEFAULT_MAX_COMPLETION_TOKENS
        CommonUtils.copy_py_int_to_c_field(llm_service.ffi, query, 'max_completion_tokens', max_tokens)
        CommonUtils.copy_py_float_to_c_field(llm_service.ffi, query, 'temperature',
            request_data.temperature or QUERY_CONST.DEFAULT_TEMPERATURE)
        CommonUtils.copy_py_float_to_c_field(llm_service.ffi, query, 'top_p',
            request_data.top_p or QUERY_CONST.DEFAULT_TOP_P)
        CommonUtils.copy_py_float_to_c_field(llm_service.ffi, query, 'frequency_penalty',
            request_data.frequency_penalty or QUERY_CONST.DEFAULT_FREQUENCY_PENALTY)
        CommonUtils.copy_py_float_to_c_field(llm_service.ffi, query, 'presence_penalty',
            request_data.presence_penalty or QUERY_CONST.DEFAULT_PRESENCE_PENALTY)

        logger.info(f"Returning completion_id: {completion_id}")
        return llm_service, handle, query, completion_id

    @staticmethod
    def chat_increment_message_count(completion_id: str = None):
        """
        Increment the message count and message pairs count for given chat id.
        Also updates total_conversation_tokens.
        """
        map_obj = HandleIdObjectMap()
        handle_obj = map_obj.get_handle(completion_id)
        if handle_obj:
            handle_obj.messages_count = handle_obj.messages_count + 1
            # Increment message pairs count (every 2 messages = 1 pair)
            if handle_obj.messages_count % 2 == 0:
                handle_obj.message_pairs_count = handle_obj.message_pairs_count + 1
                logger.debug(f"Message pair count incremented to {handle_obj.message_pairs_count} for completion_id {completion_id}")
            map_obj.set_handle(handle_obj, completion_id)
