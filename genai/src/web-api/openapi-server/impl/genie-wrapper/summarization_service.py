# Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
# SPDX-License-Identifier: BSD-3-Clause-Clear

from typing import List
from openapi_server.impl.conversation_tracker import ConversationMessage
from openapi_server.impl.genie_wrapper.gen_ai_service_singleton import LLMService
from openapi_server.impl.genie_wrapper.utils.token_counter import TokenCounter
from openapi_server.impl.genie_wrapper.utils.common_utils import CommonUtils
from openapi_server.impl.constant import LLMServiceKeys
from openapi_server.logger.logger_config import LoggerConfig
import logging

LoggerConfig.initialize(level=logging.DEBUG)
logger = LoggerConfig.get_logger(__name__)


class SummarizationService:
    """
    Service to handle conversation summarization.
    """

    SUMMARIZATION_PROMPT_TEMPLATE = """Please provide a concise summary of the following conversation. Focus on key points, decisions, and important information. Keep the summary under 400 tokens.

Conversation:
{conversation_history}"""

    @staticmethod
    def build_summarization_prompt(messages: List[ConversationMessage]) -> str:
        """
        Build a summarization prompt from conversation history.

        Args:
            messages: List of conversation messages

        Returns:
            Formatted summarization prompt
        """
        conversation_text = ""
        for msg in messages:
            conversation_text += f"{msg.role.capitalize()}: {msg.content}\n\n"

        prompt = SummarizationService.SUMMARIZATION_PROMPT_TEMPLATE.format(
            conversation_history=conversation_text.strip()
        )

        logger.debug(f"Built summarization prompt with {len(messages)} messages")
        return prompt

    @staticmethod
    def summarize_conversation(
        messages: List[ConversationMessage],
        model_str: str,
        llm_handle,
        max_summary_tokens: int = 400
    ) -> tuple[str, int]:
        """
        Generate a summary of the conversation using the LLM.

        Args:
            messages: List of conversation messages to summarize
            model_str: Model identifier
            llm_handle: Pre-existing LLM handle to reuse
            max_summary_tokens: Maximum tokens for the summary

        Returns:
            Tuple of (summary_text, token_count)
        """
        try:
            logger.info(f"Starting summarization for {len(messages)} messages")

            # Build summarization prompt
            summarization_prompt = SummarizationService.build_summarization_prompt(messages)

            logger.info("=== SUMMARIZATION PROMPT START ===")
            logger.info(summarization_prompt)
            logger.info("=== SUMMARIZATION PROMPT END ===")

            llm_service = LLMService()
            # Prepare query for summarization
            query = llm_service.ffi.new(LLMServiceKeys.QUERY)

            # Set query parameters
            CommonUtils.copy_py_string_to_c_array(
                llm_service.ffi,
                query.model,
                model_str,
                256
            )
            CommonUtils.copy_py_string_to_c_array(
                llm_service.ffi,
                query.message.role,
                "user",
                256
            )
            CommonUtils.copy_py_string_to_c_array(
                llm_service.ffi,
                query.message.content,
                summarization_prompt,
                12300
            )

            # Set max tokens for summary
            CommonUtils.copy_py_int_to_c_field(
                llm_service.ffi,
                query,
                'max_completion_tokens',
                max_summary_tokens if max_summary_tokens is not None else 400
            )

            # Set other parameters to defaults
            CommonUtils.copy_py_float_to_c_field(llm_service.ffi, query, 'temperature', 0.7)
            CommonUtils.copy_py_float_to_c_field(llm_service.ffi, query, 'top_p', 1.0)
            CommonUtils.copy_py_float_to_c_field(llm_service.ffi, query, 'frequency_penalty', 0.0)
            CommonUtils.copy_py_float_to_c_field(llm_service.ffi, query, 'presence_penalty', 0.0)

            # Container for summary text
            summary_text = []

            @llm_service.ffi.callback("void(const Response *)")
            def summary_callback(response_ptr):
                """Callback to capture summary response"""
                if response_ptr != llm_service.ffi.NULL:
                    choice = response_ptr.choices[0]
                    msg_choice = choice.message
                    content = llm_service.ffi.string(msg_choice.content).decode('utf-8')
                    if content:
                        summary_text.append(content)
                        logger.debug(f"Received summary chunk: {len(content)} chars")

            # Execute summarization using provided handle, no streaming
            llm_service.lib.llm_chat_completion_create(
                llm_handle,
                query,
                False,
                summary_callback
            )

            # Combine summary text
            final_summary = ''.join(summary_text).strip()

            logger.info("=== SUMMARY RESULT START ===")
            logger.info(final_summary)
            logger.info("=== SUMMARY RESULT END ===")

            if not final_summary:
                logger.warning("Summarization returned empty result, using fallback")
                final_summary = "Previous conversation context summarized."
                logger.info("=== SUMMARY RESULT START ===")
                logger.info(final_summary)
                logger.info("=== SUMMARY RESULT END ===")

            # Estimate token count of summary
            summary_token_count = TokenCounter.estimate_tokens(final_summary)

            logger.info(
                f"Summarization complete: {len(final_summary)} chars, "
                f"~{summary_token_count} tokens"
            )

            return final_summary, summary_token_count

        except Exception as e:
            logger.error(f"Error during summarization: {e}", exc_info=True)
            # Return a fallback summary
            fallback_summary = "Previous conversation context (summarization failed)."
            logger.info("=== SUMMARY RESULT START ===")
            logger.info(fallback_summary)
            logger.info("=== SUMMARY RESULT END ===")
            return fallback_summary, TokenCounter.estimate_tokens(fallback_summary)
