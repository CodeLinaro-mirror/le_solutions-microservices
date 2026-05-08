# Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
# SPDX-License-Identifier: BSD-3-Clause-Clear

"""
ConversationUtils: Helper utilities for conversation processing.
Includes hash calculation for session tracking and message content extraction.
"""

import hashlib
import json
from typing import List, Dict, Any, Union
from openapi_server.logger.logger_config import LoggerConfig

LoggerConfig.initialize()
logger = LoggerConfig.get_logger(__name__)

class ConversationUtils:
    @staticmethod
    def safe_get(obj: Any, key: str, default: Any = None) -> Any:
        """
        Safely get attribute from dict or Pydantic model/object.

        Args:
            obj: Dictionary or object
            key: Key or attribute name
            default: Default value if not found

        Returns:
            Value of the key/attribute or default
        """
        if isinstance(obj, dict):
            return obj.get(key, default)
        return getattr(obj, key, default)

    @staticmethod
    def calculate_hash_for_specific_messages(messages: List[Union[Dict, Any]], debug: bool = False) -> str:
        """
        Calculate hash for a specific set of messages.
        Used for event-based session matching.

        Args:
            messages: List of message dictionaries or objects
            debug: If True, log detailed hash calculation info

        Returns:
            16-character hex hash string
        """
        if debug:
            return ConversationUtils.debug_hash_calculation(messages, "Specific Messages")

        conversation_str = ""
        for msg in messages:
            role = ConversationUtils.safe_get(msg, "role", "")
            content = ConversationUtils.get_content_string(msg)

            # Handle tool calls
            tool_calls_info = ""
            if ConversationUtils.has_tool_calls(msg):
                tool_info = ConversationUtils.get_tool_call_info(msg)
                if tool_info:
                    tool_calls_info = f"|tools:{tool_info}"

            conversation_str += f"{role}:{content}{tool_calls_info}|"

        return hashlib.sha256(conversation_str.encode()).hexdigest()[:16]

    @staticmethod
    def debug_hash_calculation(messages: List[Union[Dict, Any]], label: str = "") -> str:
        """
        Calculate hash with detailed logging for debugging.

        Args:
            messages: List of message dictionaries or objects
            label: Label for this hash calculation (for logging)

        Returns:
            16-character hex hash string
        """
        logger.info(f"=== Hash Calculation Debug: {label} ===")
        conversation_str = ""

        for idx, msg in enumerate(messages):
            role = ConversationUtils.safe_get(msg, "role", "")
            content = ConversationUtils.get_content_string(msg)

            logger.info(f"Message {idx}:")
            logger.info(f"  Role: {role}")
            # Use safe_get for logging raw content too
            raw_content = ConversationUtils.safe_get(msg, "content")
            logger.info(f"  Raw content type: {type(raw_content)}")
            logger.info(f"  Normalized content: {content[:100]}...")

            # Handle tool calls
            tool_calls_info = ""
            if ConversationUtils.has_tool_calls(msg):
                tool_info = ConversationUtils.get_tool_call_info(msg)
                if tool_info:
                    tool_calls_info = f"|tools:{tool_info}"
                    logger.info(f"  Tool calls: {tool_info}")

            conversation_str += f"{role}:{content}{tool_calls_info}|"

        hash_result = hashlib.sha256(conversation_str.encode()).hexdigest()[:16]
        logger.info(f"Final hash string: {conversation_str[:200]}...")
        logger.info(f"Hash result: {hash_result}")
        logger.info("=== End Hash Debug ===")

        return hash_result

    @staticmethod
    def get_content_string(msg: Union[Dict, Any]) -> str:
        """
        Helper method to extract content as string from a message dictionary or object,
        handling text, objects, and image content types for proper hash calculation.

        For images: Uses URL if available, otherwise samples ~256 chars from base64.
        This ensures consistent hashing for session tracking with multimodal content.

        Args:
            msg: Message dictionary or object
        Returns:
            String representation of content including image identifiers
        """
        content = ConversationUtils.safe_get(msg, "content")

        if content is None:
            return ""

        if isinstance(content, str):
            return content.strip()

        # Handle list content (multimodal messages with text and images)
        if isinstance(content, list):
            parts = []
            for item in content:
                # Item can also be a dict or object
                item_type = ConversationUtils.safe_get(item, 'type')

                if item_type == 'text':
                    text_content = ConversationUtils.safe_get(item, 'text', '')
                    if text_content:
                        parts.append(text_content.strip())

                elif item_type == 'image_url':
                    image_url_data = ConversationUtils.safe_get(item, 'image_url', {})
                    if isinstance(image_url_data, dict):
                        url = image_url_data.get('url', '')
                    else:
                        url = getattr(image_url_data, 'url', str(image_url_data))

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

            return " ".join(parts).strip()

        # Fallback: convert object to string representation
        return str(content)

    @staticmethod
    def has_tool_calls(msg: Union[Dict, Any]) -> bool:
        """
        Helper method to check if a message has tool calls.

        Args:
            msg: Message dictionary or object
        Returns:
            Boolean indicating if message has tool calls
        """
        tool_calls = ConversationUtils.safe_get(msg, "tool_calls")
        return tool_calls is not None and len(tool_calls) > 0

    @staticmethod
    def get_tool_call_info(msg: Union[Dict, Any]) -> str:
        """
        Helper method to extract tool call information from a message.

        Args:
            msg: Message dictionary or object with tool calls
        Returns:
            String representation of tool call information
        """
        if not ConversationUtils.has_tool_calls(msg):
            return ""

        tool_calls = ConversationUtils.safe_get(msg, "tool_calls")
        tool_info = []

        for tc in tool_calls:
            # Handle both dict and object access (if Pydantic models passed)
            if isinstance(tc, dict):
                function = tc.get("function", {})
                name = function.get("name", "") if isinstance(function, dict) else getattr(function, "name", "")
                tc_id = tc.get("id", "")
            else:
                function = getattr(tc, "function", None)
                name = getattr(function, "name", "") if function else ""
                tc_id = getattr(tc, "id", "")

            if name and tc_id:
                tool_info.append(f"{name}:{tc_id}")

        return "|".join(tool_info)

    @staticmethod
    def calculate_conversation_hash(messages: List[Union[Dict, Any]], exclude_last_pair: bool = True) -> str:
        """
        Calculate deterministic hash of complete user-assistant pairs only,
        including tool calling sequences.

        This ensures consistent hashing across requests as the conversation grows.
        System messages are excluded from hash calculation as they are context, not conversation.

        Tool calling sequences (user → assistant(tool_call) → tool → assistant(response))
        are treated as a single complete pair. Incomplete tool sequences are handled specially.

        Args:
            messages: List of message dictionaries or objects
            exclude_last_pair: If True, exclude the last complete pair
        Returns:
            16-character hex hash string, or empty string if no complete pairs to hash
        """
        # Filter out system messages before calculating hash
        # Use safe_get to handle both dicts and objects
        conversation_messages = [
            msg for msg in messages
            if ConversationUtils.safe_get(msg, "role") != "system"
        ]

        complete_pairs = []
        incomplete_sequence = None
        i = 0

        while i < len(conversation_messages):
            msg_role = ConversationUtils.safe_get(conversation_messages[i], "role")

            if msg_role == "user":
                user_msg = conversation_messages[i]
                i += 1

                # Collect ALL assistant and tool messages until next user message
                assistant_sequence = []
                while i < len(conversation_messages):
                    curr_role = ConversationUtils.safe_get(conversation_messages[i], "role")
                    if curr_role == "user":
                        break
                    assistant_sequence.append(conversation_messages[i])
                    i += 1

                # If we have assistant messages, check if this is a complete pair
                if assistant_sequence:
                    # Find the FINAL assistant message (last one with content)
                    final_assistant = None
                    for msg in reversed(assistant_sequence):
                        role = ConversationUtils.safe_get(msg, "role")
                        if role == "assistant" and ConversationUtils.get_content_string(msg):
                            final_assistant = msg
                            break

                    if final_assistant:
                        # This is a COMPLETE pair - has final assistant response
                        tool_calls_info = []
                        for msg in assistant_sequence:
                            role = ConversationUtils.safe_get(msg, "role")
                            if role == "assistant" and ConversationUtils.has_tool_calls(msg):
                                tool_info = ConversationUtils.get_tool_call_info(msg)
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
                            ConversationUtils.safe_get(msg, "role") == "assistant" and
                            ConversationUtils.has_tool_calls(msg)
                            for msg in assistant_sequence
                        )

                        if has_tool_call:
                            # Store as incomplete sequence for special handling
                            incomplete_sequence = {
                                'user': user_msg,
                                'sequence': assistant_sequence
                            }
                            user_content = ConversationUtils.get_content_string(user_msg)
                            logger.debug(f"Found incomplete tool calling sequence for user: {user_content[:50]}...")
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
            user_content = ConversationUtils.get_content_string(pair['user'])
            asst_content = ConversationUtils.get_content_string(pair['assistant'])

            # Include tool call info for determinism
            if pair['tool_calls']:
                tool_info = "|".join(pair['tool_calls'])
                conversation_str += f"user:{user_content}|tools:{tool_info}|assistant:{asst_content}|"
            else:
                conversation_str += f"user:{user_content}|assistant:{asst_content}|"

        hash_result = hashlib.sha256(conversation_str.encode()).hexdigest()[:16]
        logger.debug(f"Calculated hash for {len(pairs_to_hash)} complete pairs: {hash_result}")
        return hash_result
