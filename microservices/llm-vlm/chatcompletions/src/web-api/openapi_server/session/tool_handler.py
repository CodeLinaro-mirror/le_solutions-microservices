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
    def format_tools_for_prompt(
        tools: List[ChatCompletionTool],
        ceiling_tokens: int = None,
        model_id: str = None,
    ) -> str:
        """
        Convert tools to a compact text format the model can understand.

        Uses a three-level truncation strategy to stay within ceiling_tokens:
          Level 1: Compact signature format with parameter descriptions (~35-60 tokens/tool)
          Level 2: Compact format without parameter descriptions, truncated function descriptions
          Level 3: Drop tools from end of list until under ceiling

        Args:
            tools: List of ChatCompletionTool objects
            ceiling_tokens: Hard token ceiling for the entire tools block.
                           Defaults to SLOT_TOOLS_CEILING from constants.
            model_id: Reserved for future model-specific format selection (currently unused).

        Returns:
            Formatted string describing the tools and usage instructions,
            guaranteed to be within ceiling_tokens.
        """
        if not tools:
            return ""

        from openapi_server.impl.constant import SLOT_TOOLS_CEILING
        from openapi_server.session.token_counter import TokenCounter
        ceiling = ceiling_tokens if ceiling_tokens is not None else SLOT_TOOLS_CEILING

        # Fixed header: usage instructions (always included).
        # IMPORTANT: Use {...} as the arguments placeholder — NOT a concrete key-value
        # example like {"param": "value"}. A concrete example causes the model to copy
        # the placeholder key name literally (e.g. outputting {"param": "Turin"} instead
        # of {"location": "Turin"}). The {...} form is clearly a placeholder and the
        # model correctly substitutes the real parameter names from the tool signature.
        header = (
            "\n\n--- TOOLS ---\n"
            "You have access to the following tools. When the user's request requires "
            "information that a tool can provide, you MUST call the appropriate tool.\n"
            "To call a tool, respond ONLY with this JSON (no other text before or after):\n"
            '{"tool_calls": [{"type": "function", "function": {"name": "TOOL_NAME", "arguments": {...}}}]}\n'
            "Replace TOOL_NAME with the tool name and {...} with the actual arguments as JSON "
            "using the parameter names shown in the tool signature below.\n"
            "If no tool is needed to answer the question, respond with plain text only "
            "(do NOT output JSON).\n\n"
        )

        def _build_signature(func, include_param_desc: bool) -> str:
            """Build compact function signature: name(param*: type, ...) → description"""
            params = func.parameters or {}
            props = params.get("properties", {}) if isinstance(params, dict) else {}
            required = params.get("required", []) if isinstance(params, dict) else []
            sig_parts = []
            for pname, pdef in props.items():
                ptype = pdef.get("type", "any") if isinstance(pdef, dict) else "any"
                marker = "*" if pname in required else ""
                if include_param_desc and isinstance(pdef, dict):
                    pdesc = pdef.get("description", "")
                    if pdesc:
                        sig_parts.append(f"{pname}{marker}: {ptype} ({pdesc[:40]})")
                        continue
                sig_parts.append(f"{pname}{marker}: {ptype}")
            return f"{func.name}({', '.join(sig_parts)})"

        def _render(tool_list: list, include_param_desc: bool, truncate_desc: bool) -> str:
            lines = []
            for tool in tool_list:
                func = tool.function
                sig = _build_signature(func, include_param_desc)
                desc = func.description or ""
                if truncate_desc and len(desc) > 60:
                    desc = desc[:57] + "..."
                lines.append(f"- {sig} → {desc}")
            return header + "\n".join(lines)

        # Level 1: compact with parameter descriptions
        text = _render(list(tools), include_param_desc=True, truncate_desc=False)
        if TokenCounter.estimate_tokens(text) <= ceiling:
            return text

        # Level 2: compact without parameter descriptions, truncated function descriptions
        text = _render(list(tools), include_param_desc=False, truncate_desc=True)
        if TokenCounter.estimate_tokens(text) <= ceiling:
            return text

        # Level 3: drop tools from end until under ceiling
        tool_list = list(tools)
        while tool_list:
            text = _render(tool_list, include_param_desc=False, truncate_desc=True)
            if TokenCounter.estimate_tokens(text) <= ceiling:
                if len(tool_list) < len(tools):
                    dropped_names = [t.function.name for t in tools[len(tool_list):]]
                    logger.warning(
                        f"Tool instructions exceed budget ({ceiling} tokens): "
                        f"dropped tools {dropped_names}"
                    )
                return text
            tool_list.pop()

        # Fallback: header only (no tools fit)
        logger.warning(f"No tools fit within budget ({ceiling} tokens): returning header only")
        return header

    @staticmethod
    def inject_tool_instructions(
        messages: List[Any],
        tools: List[ChatCompletionTool],
        model_id: str = None,
    ) -> List[Any]:
        """
        Inject tool instructions into the messages.
        - If a system message exists: append tool instructions to it
        - If no system message: create one with tool instructions

        Handles both Pydantic model objects (from request parsing) and plain
        dicts (from session history). Always creates new system messages as
        plain dicts to avoid AttributeError when downstream code calls .get().

        Args:
            messages: List of message objects (Pydantic models or dicts)
            tools: List of ChatCompletionTool objects
            model_id: Reserved for future model-specific format selection (currently unused).

        Returns:
            Modified list of messages with tool instructions
        """
        if not tools:
            return messages

        tool_instructions = ToolHandler.format_tools_for_prompt(tools, model_id=model_id)

        # Check if first message is a system message.
        # Support both Pydantic objects (.role attribute) and dicts (['role'] key).
        def _get_role(msg) -> str:
            if isinstance(msg, dict):
                return msg.get('role', '')
            return getattr(msg, 'role', '')

        def _get_content(msg) -> str:
            if isinstance(msg, dict):
                return msg.get('content') or ''
            return getattr(msg, 'content', '') or ''

        def _set_content(msg, content: str):
            if isinstance(msg, dict):
                msg['content'] = content
            else:
                msg.content = content

        has_system_msg = bool(messages) and _get_role(messages[0]) == "system"

        if has_system_msg:
            # Append to existing system message (works for both Pydantic and dict)
            existing_content = _get_content(messages[0])
            _set_content(messages[0], f"{existing_content}{tool_instructions}")
            logger.info("Appended tool instructions to existing system message")
        else:
            # Create new system message as a plain dict.
            # Using a Pydantic ChatCompletionRequestSystemMessage here causes
            # AttributeError in text_conversation_event.py which calls .get()
            # on messages (dict method, not available on Pydantic objects).
            system_msg = {
                "role": "system",
                "content": f"You are a helpful assistant.{tool_instructions}"
            }
            messages.insert(0, system_msg)
            logger.info("Created new system message with tool instructions")

        return messages

    @staticmethod
    def parse_tool_response(response_text: str) -> Optional[List[ChatCompletionMessageToolCall]]:
        """
        Parse model response to detect if it wants to call a function.
        Looks for JSON patterns that indicate function calls in OpenAI format,
        as well as the <tool_call> XML format used by some model variants.

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
                    # Empty tool_calls array — model signalled "no tool needed" in JSON form.
                    # Return None so the caller treats this as a regular text response.
                    if isinstance(tool_calls_data, list) and len(tool_calls_data) == 0:
                        logger.debug(
                            "Model returned empty tool_calls array — treating as no-tool response"
                        )
                        return None

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

            # Strategy 4: Handle {"type": "function", "function": {"name": ..., "arguments": ...}}
            # This is the format the model sometimes outputs directly.
            try:
                stripped = response_text.strip()
                parsed = json.loads(stripped)
                if (isinstance(parsed, dict)
                        and parsed.get("type") == "function"
                        and "function" in parsed):
                    func_data = parsed["function"]
                    tool_calls_data = [{
                        "id": f"call_{uuid.uuid4().hex[:24]}",
                        "type": "function",
                        "function": func_data,
                    }]
                    func_name = func_data.get("name", "unknown")
                    logger.info(f"Detected function call (Strategy 4 - type/function): {func_name}")
                    return ToolHandler._convert_to_tool_calls(tool_calls_data)
            except (json.JSONDecodeError, AttributeError):
                pass

            # Strategy 5: <tool_call> XML format
            # Some model variants respond with:
            #   <tool_call>
            #   {"name": "func_name", "arguments": {"param": "value"}}
            #   </tool_call>
            tool_call_xml_pattern = r'<tool_call>\s*([\s\S]*?)\s*</tool_call>'
            xml_matches = re.findall(tool_call_xml_pattern, response_text)
            if xml_matches:
                tool_calls_data = []
                for match_text in xml_matches:
                    try:
                        parsed = json.loads(match_text.strip())
                        if isinstance(parsed, dict) and 'name' in parsed:
                            tool_calls_data.append({
                                "id": f"call_{uuid.uuid4().hex[:24]}",
                                "type": "function",
                                "function": {
                                    "name": parsed.get("name", ""),
                                    "arguments": parsed.get("arguments", {})
                                }
                            })
                    except json.JSONDecodeError:
                        logger.debug(
                            f"Failed to parse <tool_call> content as JSON: {match_text[:100]}"
                        )
                if tool_calls_data:
                    func_names = [tc["function"]["name"] for tc in tool_calls_data]
                    logger.info(
                        f"Detected {len(tool_calls_data)} tool call(s) "
                        f"(Strategy 5 - <tool_call> XML): {func_names}"
                    )
                    return ToolHandler._convert_to_tool_calls(tool_calls_data)

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
            '"type": "function"',
            '<tool_call>',
        ]

        response_lower = response_text.lower()
        return any(indicator.lower() in response_lower for indicator in indicators)
