# Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
# SPDX-License-Identifier: BSD-3-Clause-Clear

import json
import re
import uuid
from typing import List, Optional, Dict, Any

from openapi_server.models.chat_completion_tool import ChatCompletionTool
from openapi_server.models.chat_completion_message_tool_call import ChatCompletionMessageToolCall
from openapi_server.models.chat_completion_message_tool_call_function import ChatCompletionMessageToolCallFunction
from openapi_server.logger.logger_config import LoggerConfig

LoggerConfig.initialize()
logger = LoggerConfig.get_logger(__name__)

class ToolHandler:
    @staticmethod
    def format_tools_for_prompt(tools: List[ChatCompletionTool]) -> str:
        """
        Convert tools to a text format that the model can understand.
        Creates instructions for the LLM on how to use tools.

        Args:
            tools: List of ChatCompletionTool objects

        Returns:
            Formatted string describing the tools and usage instructions
        """
        if not tools:
            return ""

        tools_text = "\n\n--- TOOL USAGE GUIDELINES ---\n\n"
        tools_text += "IMPORTANT: You can ONLY use the tools explicitly listed below. "
        tools_text += "If no relevant tool is available for the user's request, respond with text instead of calling any function. "
        tools_text += "Do NOT output a JSON object with empty tool_calls. If no tool is needed, simply output the plain text response. "
        tools_text += "Never invent or hallucinate function names that are not in the tools list. "
        tools_text += "If the available tools cannot directly help with the user request, do not call any tool and reply with plain text instead.\n"

        tools_text += "\n--- AVAILABLE TOOLS ---\n\n"
        tools_text += (
            "You have access to the following tools. To use a tool, respond with a JSON object in this exact format:\n\n"
        )
        tools_text += (
            '{\n  "tool_calls": [{\n    "type": "function",\n    "function": {\n      "name": "tool_name",\n'
            '      "arguments": {\n        "param": "value"\n      }\n    }\n  }]\n}\n\n'
        )
        tools_text += "Available tools:\n\n"

        for i, tool in enumerate(tools, 1):
            func = tool.function
            tools_text += f"{i}. {func.name}\n"

            if func.description:
                tools_text += f"   Description: {func.description}\n"

            if func.parameters:
                try:
                    params_str = json.dumps(func.parameters, indent=2)
                    tools_text += f"   Parameters: {params_str}\n"
                except Exception as e:
                    logger.warning(f"Failed to format parameters for {func.name}: {e}")
                    tools_text += f"   Parameters: {func.parameters}\n"

            tools_text += "\n"

        return tools_text

    def inject_tool_instructions(messages: List[Any], tools: List[ChatCompletionTool]) -> List[Any]:
        """
        Inject tool instructions into the messages.
        - If a system message exists: append tool instructions to it
        - If no system message: create one with tool instructions

        Args:
            messages: List of message objects
            tools: List of ChatCompletionTool objects

        Returns:
            Modified list of messages with tool instructions
        """
        if not tools:
            return messages

        tool_instructions = ToolHandler.format_tools_for_prompt(tools)

        # Check if first message is a system message
        has_system_msg = messages and hasattr(messages[0], 'role') and messages[0].role == "system"

        if has_system_msg:
            # Append to existing system message
            existing_content = messages[0].content or ""
            messages[0].content = f"{existing_content}{tool_instructions}"
            logger.info("Appended tool instructions to existing system message")
        else:
            # Create new system message with tool instructions
            from openapi_server.models.chat_completion_request_system_message import ChatCompletionRequestSystemMessage

            system_msg = ChatCompletionRequestSystemMessage(
                role="system",
                content=f"You are a helpful assistant.{tool_instructions}"
            )
            messages.insert(0, system_msg)
            logger.info("Created new system message with tool instructions")

        return messages

    @staticmethod
    def parse_tool_response(response_text: str) -> Optional[List[ChatCompletionMessageToolCall]]:
        """
        Parse model response to detect if it wants to call a function.
        Looks for JSON patterns that indicate function calls in OpenAI format.

        Args:
            response_text: The text response from the model

        Returns:
            List of ChatCompletionMessageToolCall objects if function calls detected, None otherwise
        """
        if not response_text:
            return None

        try:
            # Strategy 1: Try to parse entire response as JSON
            try:
                parsed = json.loads(response_text.strip())

                # Check for OpenAI format: {"tool_calls": [...]}
                if isinstance(parsed, dict) and "tool_calls" in parsed:
                    tool_calls_data = parsed["tool_calls"]
                    if isinstance(tool_calls_data, list) and len(tool_calls_data) > 0:
                        return ToolHandler._convert_to_tool_calls(tool_calls_data)

            except json.JSONDecodeError:
                pass

            # Strategy 2: Look for JSON object containing tool_calls
            # This handles cases where LLM adds extra text around the JSON
            tool_calls_pattern = r'\{[\s\S]*?"tool_calls"[\s\S]*?\[[\s\S]*?\][\s\S]*?\}'
            match = re.search(tool_calls_pattern, response_text)

            if match:
                try:
                    parsed = json.loads(match.group())
                    if "tool_calls" in parsed:
                        tool_calls_data = parsed["tool_calls"]
                        if isinstance(tool_calls_data, list) and len(tool_calls_data) > 0:
                            return ToolHandler._convert_to_tool_calls(tool_calls_data)
                except json.JSONDecodeError:
                    pass

            # Strategy 3: Look for simpler function call format
            # {"function": {"name": "...", "arguments": "..."}}
            function_pattern = r'\{[\s\S]*?"function"[\s\S]*?\{[\s\S]*?"name"[\s\S]*?"arguments"[\s\S]*?\}[\s\S]*?\}'
            match = re.search(function_pattern, response_text)

            if match:
                try:
                    parsed = json.loads(match.group())
                    if "function" in parsed:
                        # Wrap in tool_calls format for consistency
                        tool_calls_data = [{
                            "id": f"call_{uuid.uuid4().hex[:24]}",
                            "type": "function",
                            "function": parsed["function"]
                        }]
                        func_name = parsed['function'].get('name', 'unknown')
                        logger.info(f"Detected function call (Strategy 3): {func_name}")
                        return ToolHandler._convert_to_tool_calls(tool_calls_data)
                except json.JSONDecodeError:
                    pass

        except Exception as e:
            logger.warning(f"Error parsing tool response: {e}")

        return None

    @staticmethod
    def _convert_to_tool_calls(tool_calls_data: List[Dict[str, Any]]) -> List[ChatCompletionMessageToolCall]:
        """
        Convert parsed tool_calls data to ChatCompletionMessageToolCall objects.

        Args:
            tool_calls_data: List of dictionaries containing tool call information

        Returns:
            List of ChatCompletionMessageToolCall objects
        """
        tool_calls = []

        for tc_data in tool_calls_data:
            try:
                # Extract function data
                func_data = tc_data.get("function", {})

                # Ensure arguments are valid JSON and normalized as a JSON string.
                arguments = ToolHandler._normalize_arguments(func_data.get("arguments", "{}"))
                if arguments is None:
                    raise ValueError(
                        f"Invalid function arguments for tool '{func_data.get('name', 'unknown')}'"
                    )

                tool_call = ChatCompletionMessageToolCall(
                    id=tc_data.get("id", f"call_{uuid.uuid4().hex[:24]}"),
                    type=tc_data.get("type", "function"),
                    function=ChatCompletionMessageToolCallFunction(
                        name=func_data.get("name", ""),
                        arguments=arguments
                    )
                )
                tool_calls.append(tool_call)
                logger.info(f"Converted tool call: {func_data.get('name')}")

            except Exception as e:
                logger.warning(f"Failed to convert tool call: {e}")
                continue

        return tool_calls if tool_calls else None

    @staticmethod
    def _normalize_arguments(arguments: Any) -> Optional[str]:
        """
        Normalize function arguments to a valid JSON string.
        Returns None when arguments cannot be normalized into valid JSON.
        """
        # Canonical form when model already emitted object arguments.
        if isinstance(arguments, dict):
            return json.dumps(arguments, ensure_ascii=False)

        if arguments is None:
            raw = "{}"
        elif isinstance(arguments, str):
            raw = arguments.strip() or "{}"
        else:
            try:
                raw = json.dumps(arguments, ensure_ascii=False)
            except (TypeError, ValueError):
                logger.warning(f"Unsupported function arguments type: {type(arguments)}")
                return None

        parsed = ToolHandler._json_loads_safe(raw)
        if parsed is None:
            repaired = ToolHandler._repair_argument_json(raw)
            parsed = ToolHandler._json_loads_safe(repaired)

        # Handle doubly-encoded JSON string payloads.
        if parsed is None and isinstance(raw, str) and raw.startswith('"') and raw.endswith('"'):
            outer = ToolHandler._json_loads_safe(raw)
            if isinstance(outer, str):
                nested = ToolHandler._json_loads_safe(outer)
                if nested is None:
                    nested = ToolHandler._json_loads_safe(ToolHandler._repair_argument_json(outer))
                parsed = nested

        if parsed is None:
            logger.warning(f"Invalid function.arguments JSON, dropping tool call: {raw}")
            return None

        return json.dumps(parsed, ensure_ascii=False)

    @staticmethod
    def _json_loads_safe(raw: str) -> Optional[Any]:
        """Best-effort JSON parsing helper."""
        try:
            return json.loads(raw)
        except (TypeError, ValueError, json.JSONDecodeError):
            return None

    @staticmethod
    def _repair_argument_json(raw: str) -> str:
        """
        Attempt minimal repairs for common malformed quote patterns in arguments.
        Example repaired input: {""location"": "Paris"} -> {"location": "Paris"}
        """
        repaired = raw.strip()

        # Strip markdown JSON fences if present.
        if repaired.startswith("```"):
            repaired = re.sub(r"^```(?:json)?\s*", "", repaired, flags=re.IGNORECASE)
            repaired = re.sub(r"\s*```$", "", repaired, flags=re.IGNORECASE)
            repaired = repaired.strip()

        # Fix doubled quotes around keys.
        repaired = re.sub(r'([{,]\s*)""([^"]+?)""\s*:', r'\1"\2":', repaired)
        # Final fallback for repeated quote artifacts.
        repaired = repaired.replace('""', '"')

        return repaired

    @staticmethod
    def should_check_for_tools(response_text: str) -> bool:
        """
        Quick check if response might contain a tool call.
        Used for optimization to avoid unnecessary parsing.

        Args:
            response_text: The text response from the model

        Returns:
            True if response might contain tool calls, False otherwise
        """
        if not response_text:
            return False

        # Look for common indicators
        indicators = [
            '"tool_calls"',
            '"function"',
            '"name"',
            '"arguments"',
            '{"type"',
            '"type": "function"'
        ]

        response_lower = response_text.lower()
        return any(indicator.lower() in response_lower for indicator in indicators)
