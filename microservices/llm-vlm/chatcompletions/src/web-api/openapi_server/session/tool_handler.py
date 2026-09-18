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


class _ToolFormatAdapter:
    """Base adapter for a model family's native tool-call convention."""

    def is_native(self) -> bool:
        return False

    def format_header_footer(self) -> tuple[str, str]:
        return "", ""

    def render_tool(self, func, description: str) -> str:
        return f"- {func.name} → {description}"

    def format_tool_response(self, tool_response: str) -> str:
        """Format a tool result for the model's continuation prompt."""
        return f"Tool result:\n{tool_response}\n\n"

    def looks_like_attempted_tool_call(self, response_text: str) -> bool:
        return ToolHandler.should_check_for_tools(response_text)

    def parse_response(
        self,
        response_text: str,
        tools: Optional[List[ChatCompletionTool]],
        model_id: str = None,
    ) -> Optional[List[ChatCompletionMessageToolCall]]:
        return ToolHandler._parse_tool_response_common(
            response_text, tools=tools, model_id=model_id
        )

    def normalize_tool_call_entry(self, entry: Any) -> Optional[Dict[str, Any]]:
        """
        Normalize a tool-call entry to {"id", "type": "function",
        "function": {"name", "arguments"}}. Recognizes only strict
        OpenAI-canonical fields (name/arguments, or nested function.*).
        Model-specific aliases belong in subclass overrides.
        """
        if not isinstance(entry, dict):
            return None

        function_data = entry.get("function")
        if isinstance(function_data, dict):
            name = function_data.get("name")
            arguments = function_data.get("arguments")
        else:
            name = entry.get("name")
            arguments = entry.get("arguments")

        if not isinstance(name, str) or not name.strip():
            return None

        return {
            "id": entry.get("id", f"call_{uuid.uuid4().hex[:24]}"),
            "type": "function",
            "function": {
                "name": name.strip(),
                "arguments": arguments if arguments is not None else {},
            },
        }


class _QwenToolFormatAdapter(_ToolFormatAdapter):
    """Native tool format shared by Qwen2.5/Qwen3 families."""

    def is_native(self) -> bool:
        return True

    def format_header_footer(self) -> tuple[str, str]:
        return (
            "\n\n--- TOOLS ---\n"
            "The available tools are listed between <tools> and </tools>.\n"
            "<tools>\n",
            "\n</tools>\n"
            "Only call a tool from the list above. Never invent a tool name or "
            "parameter. If a listed tool can satisfy the user's request, you MUST "
            "call it rather than answer from memory. When a tool is required, "
            "respond ONLY with a <tool_call> block (no other text):\n"
            '<tool_call>{"name":"TOOL_NAME","arguments":{}}</tool_call>\n'
            "Use the exact parameter names and types from the tool definition. "
            "If no tool is needed, respond with plain text.\n\n",
        )

    def render_tool(self, func, description: str) -> str:
        descriptor = {
            "type": "function",
            "function": {
                "name": func.name,
                "description": description,
                "parameters": func.parameters or {},
            },
        }
        return json.dumps(descriptor, ensure_ascii=False, separators=(",", ":"))

    def looks_like_attempted_tool_call(self, response_text: str) -> bool:
        # Use substring matching (not startswith) so that any preamble text
        # the model emits before the actual native call marker (a hallucinated
        # word, a filler phrase, etc.) does not defeat detection. Native
        # markers are distinctive enough that substring matching does not
        # produce meaningful false positives on ordinary prose.
        text = (response_text or "").lower()
        return (
            "<tool_call" in text
            or '"tool_calls"' in text
            or '"function"' in text
            or ('"name"' in text and '"arguments"' in text)
        )

    def normalize_tool_call_entry(self, entry: Any) -> Optional[Dict[str, Any]]:
        """
        Qwen override: also accepts "action"/"tool" as name aliases and
        "parameters"/"args" as argument aliases (observed on Qwen3-VL
        instruct checkpoints), in addition to the strict base conventions.
        """
        if not isinstance(entry, dict):
            return None

        function_data = entry.get("function")
        if isinstance(function_data, dict):
            name = function_data.get("name")
            arguments = function_data.get("arguments")
        else:
            name = (
                entry.get("name")
                or entry.get("action")
                or entry.get("tool")
            )
            if "arguments" in entry:
                arguments = entry.get("arguments")
            elif "parameters" in entry:
                arguments = entry.get("parameters")
            else:
                arguments = entry.get("args")

        if not isinstance(name, str) or not name.strip():
            return None

        return {
            "id": entry.get("id", f"call_{uuid.uuid4().hex[:24]}"),
            "type": "function",
            "function": {
                "name": name.strip(),
                "arguments": arguments if arguments is not None else {},
            },
        }


class _GemmaToolFormatAdapter(_ToolFormatAdapter):
    """Native Gemma tool format using function-call blocks."""

    def is_native(self) -> bool:
        return True

    def format_header_footer(self) -> tuple[str, str]:
        return (
            "\n\n<|tool>\n",
            "<tool|>\n"
            "Only call a tool from the list above. Never invent a tool name or "
            "parameter. If a listed tool can satisfy the user's request, you MUST "
            "call it. Respond ONLY with:\n"
            "<|tool_call>\nTOOL_NAME(arg=<|\"|>value<|\"|>)\n<tool_call|>\n"
            "Use exact parameter names and types. If no tool is needed, use plain text.\n\n",
        )

    def render_tool(self, func, description: str) -> str:
        lines = [f"{func.name}:", f"  description: {description}", "  arguments:"]
        parameters = func.parameters or {}
        properties = parameters.get("properties", {}) if isinstance(parameters, dict) else {}
        for name, definition in properties.items():
            value_type = definition.get("type", "any") if isinstance(definition, dict) else "any"
            lines.append(f"    {name}: {value_type}")
        return "\n".join(lines)

    def format_tool_response(self, tool_response: str) -> str:
        return f"<|tool_response>\n{tool_response}\n<tool_response|>\n"

    def looks_like_attempted_tool_call(self, response_text: str) -> bool:
        text = (response_text or "").lower()
        return "<|tool_call" in text or "<tool_call|" in text

    def parse_response(
        self,
        response_text: str,
        tools: Optional[List[ChatCompletionTool]],
        model_id: str = None,
    ) -> Optional[List[ChatCompletionMessageToolCall]]:
        parsed_calls = []
        pattern = r"<\|tool_call>\s*([\s\S]*?)\s*<tool_call\|>"
        for block in re.findall(pattern, response_text or ""):
            parsed = ToolHandler._parse_gemma_call(block)
            if parsed:
                parsed_calls.append(parsed)
        if parsed_calls:
            return ToolHandler._convert_to_tool_calls(parsed_calls)
        return ToolHandler._parse_tool_response_common(
            response_text, tools=tools, model_id=model_id
        )


class _GenericToolFormatAdapter(_ToolFormatAdapter):
    """Fallback adapter for models without a registered native format."""

    def format_header_footer(self) -> tuple[str, str]:
        return (
            "\n\n--- TOOLS ---\n"
            "Only call tools from the list below. If a listed tool can satisfy "
            "the user's request, you MUST call it; never invent names or parameters.\n"
            "Use the exact parameter names and types shown below.\n"
            "To call a tool, respond ONLY with this JSON:\n"
            '{"tool_calls":[{"type":"function","function":{"name":"TOOL_NAME","arguments":{...}}}]}\n'
            "If no tool is needed, respond with plain text only.\n\n",
            "",
        )


class ToolHandler:
    _FORMAT_ADAPTERS = (
        (re.compile(r"qwen(?:2(?:\.5)?|3)", re.IGNORECASE), _QwenToolFormatAdapter()),
        (re.compile(r"gemma", re.IGNORECASE), _GemmaToolFormatAdapter()),
    )
    _DEFAULT_FORMAT_ADAPTER = _GenericToolFormatAdapter()

    @staticmethod
    def _get_format_adapter(model_id: str) -> _ToolFormatAdapter:
        # FORCE_GENERIC_TOOL_FORMAT overrides native per-model formats with
        # the generic OpenAI-JSON convention for prompt formatting.
        from openapi_server.impl.constant import FORCE_GENERIC_TOOL_FORMAT
        if FORCE_GENERIC_TOOL_FORMAT:
            return ToolHandler._DEFAULT_FORMAT_ADAPTER

        return ToolHandler._get_native_format_adapter(model_id)

    @staticmethod
    def _get_native_format_adapter(model_id: str) -> _ToolFormatAdapter:
        """
        Resolve the model-family adapter, ignoring FORCE_GENERIC_TOOL_FORMAT.
        Used as a parsing fallback since a model may emit its native format
        even when the prompt requested generic JSON.
        """
        for pattern, adapter in ToolHandler._FORMAT_ADAPTERS:
            if model_id and pattern.search(model_id):
                return adapter
        return ToolHandler._DEFAULT_FORMAT_ADAPTER

    @staticmethod
    def _parse_gemma_call(block: str) -> Optional[Dict[str, Any]]:
        match = re.match(r"^\s*([A-Za-z_][A-Za-z0-9_]*)\s*\(([\s\S]*)\)\s*$", block)
        if not match:
            return None
        function_name, argument_text = match.groups()
        arguments = {}
        for item in ToolHandler._split_gemma_arguments(argument_text):
            item = item.strip()
            if not item:
                continue
            equals = ToolHandler._find_gemma_equals(item)
            if equals is None:
                return None
            key = item[:equals].strip()
            value = item[equals + 1:].strip()
            if not re.match(r"^[A-Za-z_][A-Za-z0-9_]*$", key):
                return None
            arguments[key] = ToolHandler._parse_gemma_value(value)
        return {
            "id": f"call_{uuid.uuid4().hex[:24]}",
            "type": "function",
            "function": {"name": function_name, "arguments": arguments},
        }

    @staticmethod
    def _split_gemma_arguments(argument_text: str) -> List[str]:
        parts, start, index = [], 0, 0
        delimiter = '<|"|>'
        in_string = False
        while index < len(argument_text):
            if argument_text.startswith(delimiter, index):
                in_string = not in_string
                index += len(delimiter)
                continue
            if argument_text[index] == "," and not in_string:
                parts.append(argument_text[start:index])
                start = index + 1
            index += 1
        parts.append(argument_text[start:])
        return parts

    @staticmethod
    def _find_gemma_equals(argument_text: str) -> Optional[int]:
        delimiter = '<|"|>'
        in_string = False
        for index, char in enumerate(argument_text):
            if argument_text.startswith(delimiter, index):
                in_string = not in_string
            elif char == "=" and not in_string:
                return index
        return None

    @staticmethod
    def _parse_gemma_value(value: str) -> Any:
        delimiter = '<|"|>'
        if value.startswith(delimiter) and value.endswith(delimiter):
            return value[len(delimiter):-len(delimiter)]
        lowered = value.lower()
        if lowered == "true":
            return True
        if lowered == "false":
            return False
        if lowered in ("null", "none"):
            return None
        try:
            return int(value)
        except ValueError:
            try:
                return float(value)
            except ValueError:
                return value

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

        adapter = ToolHandler._get_format_adapter(model_id)
        header, footer = adapter.format_header_footer()

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
                desc = func.description or ""
                if truncate_desc and len(desc) > 60:
                    desc = desc[:57] + "..."
                if adapter.is_native():
                    lines.append(adapter.render_tool(func, desc))
                else:
                    sig = _build_signature(func, include_param_desc)
                    lines.append(f"- {sig} → {desc}")
            return header + "\n".join(lines) + footer

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
        return header + footer

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
    def parse_tool_response(
        response_text: str,
        tools: Optional[List[ChatCompletionTool]] = None,
        model_id: str = None,
    ) -> Optional[List[ChatCompletionMessageToolCall]]:
        """Parse native output and retain only caller-supplied tools."""
        adapter = ToolHandler._get_format_adapter(model_id)
        parsed_calls = adapter.parse_response(
            response_text, tools, model_id=model_id
        )
        if not parsed_calls or not tools:
            return parsed_calls

        # Case-insensitive lookup: some models alter tool-name casing (e.g.
        # "Get_Current_Weather" instead of "get_current_weather"). Map to the
        # caller's original casing so downstream consumers always see the
        # exact name they declared, regardless of what the model emitted.
        allowed_names = {
            getattr(getattr(tool, "function", None), "name", "").lower(): (
                getattr(getattr(tool, "function", None), "name", "")
            )
            for tool in tools
            if getattr(getattr(tool, "function", None), "name", "")
        }
        valid_calls = []
        for call in parsed_calls:
            called_name = getattr(getattr(call, "function", None), "name", "")
            original_name = allowed_names.get(called_name.lower())
            if original_name is None:
                continue
            if original_name != called_name:
                call.function.name = original_name
            valid_calls.append(call)

        rejected = len(parsed_calls) - len(valid_calls)
        if rejected:
            logger.warning(
                "Rejected %d tool call(s) for unavailable tool names",
                rejected,
            )
        return valid_calls or None

    @staticmethod
    def _parse_tool_response_common(
        response_text: str,
        tools: Optional[List[ChatCompletionTool]] = None,
        model_id: str = None,
    ) -> Optional[List[ChatCompletionMessageToolCall]]:
        """
        Parse model response to detect if it wants to call a function.
        Looks for JSON patterns that indicate function calls in OpenAI format,
        as well as the <tool_call> XML format used by some model variants.

        Args:
            response_text: The text response from the model
            tools: Optional tool definitions for native-output recovery.
            model_id: Model identifier used for logging/dispatch context.

        Returns:
            List of ChatCompletionMessageToolCall objects if function calls detected, None otherwise
        """
        if not response_text:
            return None

        # Try the forced/prompt-selected adapter's normalizer first, then
        # fall back to the model's native adapter — a model may emit its
        # native fields even when the prompt requested generic JSON.
        forced_adapter = ToolHandler._get_format_adapter(model_id)
        native_adapter = ToolHandler._get_native_format_adapter(model_id)
        candidate_normalizers = [forced_adapter.normalize_tool_call_entry]
        if native_adapter is not forced_adapter:
            candidate_normalizers.append(native_adapter.normalize_tool_call_entry)

        def normalize_entry(entry: Any) -> Optional[Dict[str, Any]]:
            for normalizer in candidate_normalizers:
                normalized = normalizer(entry)
                if normalized:
                    return normalized
            return None

        try:
            # Strategy 1: Try to parse entire response as JSON
            try:
                parsed = json.loads(response_text.strip())
            except json.JSONDecodeError:
                # Some models (observed with certain Qwen3-VL checkpoints)
                # drop exactly one trailing closing bracket/brace when
                # emitting nested tool-call JSON, even though every value is
                # otherwise complete. Attempt a structural bracket repair
                # before giving up on this strategy entirely.
                repaired_text = ToolHandler._attempt_bracket_repair(response_text.strip())
                parsed = ToolHandler._json_loads_safe(repaired_text) if repaired_text else None
                if parsed is not None:
                    logger.info(
                        "Recovered tool_calls JSON via bracket repair "
                        f"(added {len(repaired_text) - len(response_text.strip())} "
                        "closing character(s))"
                    )

            if parsed is not None:
                # Check for OpenAI format: {"tool_calls": [...]}
                if isinstance(parsed, dict) and "tool_calls" in parsed:
                    tool_calls_data = parsed["tool_calls"]
                    if isinstance(tool_calls_data, list) and len(tool_calls_data) > 0:
                        normalized_calls = [
                            normalized
                            for entry in tool_calls_data
                            if (normalized := normalize_entry(entry))
                        ]
                        if normalized_calls:
                            return ToolHandler._convert_to_tool_calls(normalized_calls)
                    # Empty tool_calls array — model signalled "no tool needed" in JSON form.
                    # Return None so the caller treats this as a regular text response.
                    if isinstance(tool_calls_data, list) and len(tool_calls_data) == 0:
                        logger.debug(
                            "Model returned empty tool_calls array — treating as no-tool response"
                        )
                        return None

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
                            normalized_calls = [
                                normalized
                                for entry in tool_calls_data
                                if (normalized := normalize_entry(entry))
                            ]
                            if normalized_calls:
                                return ToolHandler._convert_to_tool_calls(normalized_calls)
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

            # Strategy 5: Native bare call object:
            # {"name": "func_name", "arguments": {...}}
            # Some Qwen exports emit this object without the wrapper.
            try:
                stripped = response_text.strip()
                parsed = json.loads(stripped)
                if (
                    isinstance(parsed, dict)
                    and isinstance(parsed.get("name"), str)
                    and "arguments" in parsed
                ):
                    tool_calls_data = [{
                        "id": f"call_{uuid.uuid4().hex[:24]}",
                        "type": "function",
                        "function": {
                            "name": parsed["name"],
                            "arguments": parsed.get("arguments", {}),
                        },
                    }]
                    logger.info(
                        f"Detected native bare tool call: {parsed['name']}"
                    )
                    return ToolHandler._convert_to_tool_calls(tool_calls_data)
            except (json.JSONDecodeError, AttributeError):
                pass

            # Strategy 6: <tool_call> XML format
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

            if tools:
                recovered = ToolHandler._extract_schema_guided_tool_call(
                    response_text, tools
                )
                if recovered:
                    logger.info("Detected tool call using schema-guided extraction")
                    return recovered

        except Exception as e:
            logger.warning(f"Error parsing tool response: {e}")

        return None

    @staticmethod
    def _extract_schema_guided_tool_call(
        response_text: str,
        tools: List[ChatCompletionTool],
    ) -> Optional[List[ChatCompletionMessageToolCall]]:
        """
        Recover calls from JSON-like output using the tool schemas, scanning
        every balanced JSON candidate (not just the first) so multiple
        back-to-back native calls are all recovered. Deduplicated by
        (name, arguments).
        """
        tool_specs = {}
        # Map lowercased tool name -> (original name, expected parameter set).
        # Matching is case-insensitive because some quantized/fine-tuned models
        # emit tool names with different casing than the caller's schema
        # (e.g. "Get_Current_Weather" instead of "get_current_weather").
        for tool in tools:
            function = getattr(tool, "function", None)
            name = getattr(function, "name", None)
            if not name:
                continue
            parameters = getattr(function, "parameters", {}) or {}
            properties = (
                parameters.get("properties", {})
                if isinstance(parameters, dict)
                else {}
            )
            tool_specs[name.lower()] = (name, set(properties))

        if not tool_specs:
            return None

        seen = set()
        collected = []
        for candidate_text in ToolHandler._extract_balanced_json_values(response_text):
            candidate = ToolHandler._json_loads_safe(candidate_text)
            if candidate is None:
                candidate = ToolHandler._json_loads_safe(
                    ToolHandler._repair_argument_json(candidate_text)
                )
            if candidate is None:
                continue

            for name, arguments in ToolHandler._find_all_schema_guided_calls(candidate, tool_specs):
                try:
                    dedup_key = (name, json.dumps(arguments, sort_keys=True, default=str))
                except TypeError:
                    dedup_key = (name, str(arguments))
                if dedup_key in seen:
                    continue
                seen.add(dedup_key)
                collected.append({
                    "id": f"call_{uuid.uuid4().hex[:24]}",
                    "type": "function",
                    "function": {"name": name, "arguments": arguments},
                })

        if collected:
            return ToolHandler._convert_to_tool_calls(collected)
        return None

    @staticmethod
    def _extract_balanced_json_values(response_text: str) -> List[str]:
        """Extract balanced JSON objects/arrays while ignoring quoted braces."""
        candidates = []
        text = response_text or ""
        for start, char in enumerate(text):
            if char not in "{[":
                continue
            stack = []
            in_string = False
            escaped = False
            for index in range(start, len(text)):
                current = text[index]
                if in_string:
                    if escaped:
                        escaped = False
                    elif current == "\\":
                        escaped = True
                    elif current == '"':
                        in_string = False
                    continue
                if current == '"':
                    in_string = True
                elif current in "{[":
                    stack.append(current)
                elif current in "}]":
                    if not stack:
                        break
                    expected = "}" if stack[-1] == "{" else "]"
                    if current != expected:
                        break
                    stack.pop()
                    if not stack:
                        candidates.append(text[start:index + 1])
                        break
        return candidates

    @staticmethod
    def _find_schema_guided_call(value: Any, tool_specs: Dict[str, tuple]):
        """
        Find a known tool name and its schema-compatible argument object.

        tool_specs maps lowercased tool name -> (original_name, expected_param_set).

        Supports two structurally distinct native conventions, since different
        model families/fine-tunes use different shapes:
          1. Value-based: the tool name appears as a string value, with the
             arguments in a sibling (or flattened) object.
             e.g. {"name": "get_weather", "arguments": {"location": "Turin"}}
          2. Key-based: the tool name appears as the dict key itself, with the
             arguments as its value.
             e.g. {"get_weather": {"location": "Turin"}}
        Matching is case-insensitive to tolerate models that alter casing.
        """
        if isinstance(value, list):
            for item in value:
                match = ToolHandler._find_schema_guided_call(item, tool_specs)
                if match:
                    return match
            return None

        if not isinstance(value, dict):
            return None

        for key, item in value.items():
            # Convention 2: tool name used as the dict key itself.
            if isinstance(key, str) and key.lower() in tool_specs:
                original_name, expected = tool_specs[key.lower()]
                if isinstance(item, dict) and (not expected or set(item).intersection(expected) or not item):
                    return original_name, item

            # Convention 1: tool name appears as a string value.
            if isinstance(item, str) and item.lower() in tool_specs:
                original_name, expected = tool_specs[item.lower()]
                siblings = [
                    sibling for sibling_key, sibling in value.items()
                    if sibling_key != key and isinstance(sibling, dict)
                ]
                for arguments in siblings:
                    if not expected or set(arguments).intersection(expected):
                        return original_name, arguments

                flattened = {
                    candidate_key: candidate_value
                    for candidate_key, candidate_value in value.items()
                    if candidate_key != key and candidate_key in expected
                }
                if flattened or not expected:
                    return original_name, flattened

            match = ToolHandler._find_schema_guided_call(item, tool_specs)
            if match:
                return match
        return None

    @staticmethod
    def _find_all_schema_guided_calls(value: Any, tool_specs: Dict[str, tuple]) -> List[tuple]:
        """
        Like _find_schema_guided_call but finds every match by recursing
        through the full structure instead of stopping at the first — some
        models emit multiple back-to-back native call objects without a
        wrapper array.
        """
        results = []

        if isinstance(value, list):
            for item in value:
                results.extend(ToolHandler._find_all_schema_guided_calls(item, tool_specs))
            return results

        if not isinstance(value, dict):
            return results

        matched_here = False
        for key, item in value.items():
            # Convention 2: tool name used as the dict key itself.
            if isinstance(key, str) and key.lower() in tool_specs:
                original_name, expected = tool_specs[key.lower()]
                if isinstance(item, dict) and (not expected or set(item).intersection(expected) or not item):
                    results.append((original_name, item))
                    matched_here = True

            # Convention 1: tool name appears as a string value.
            if isinstance(item, str) and item.lower() in tool_specs:
                original_name, expected = tool_specs[item.lower()]
                siblings = [
                    sibling for sibling_key, sibling in value.items()
                    if sibling_key != key and isinstance(sibling, dict)
                ]
                found_sibling = False
                for arguments in siblings:
                    if not expected or set(arguments).intersection(expected):
                        results.append((original_name, arguments))
                        matched_here = True
                        found_sibling = True

                if not found_sibling:
                    flattened = {
                        candidate_key: candidate_value
                        for candidate_key, candidate_value in value.items()
                        if candidate_key != key and candidate_key in expected
                    }
                    if flattened or not expected:
                        results.append((original_name, flattened))
                        matched_here = True

        # Recurse into nested structures regardless of whether this dict
        # itself matched, so calls nested inside e.g. a wrapper object are
        # still discovered alongside sibling matches at this level.
        for item in value.values():
            if isinstance(item, (dict, list)):
                results.extend(ToolHandler._find_all_schema_guided_calls(item, tool_specs))

        return results

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
    def _attempt_bracket_repair(text: str) -> Optional[str]:
        """
        Repair JSON with missing closing brackets/braces by tracking a
        structural bracket stack (ignoring braces inside quoted strings)
        and inserting any missing closer exactly where it belongs, not just
        at the end. Handles mid-string omissions (observed on some Qwen3-VL
        checkpoints) that a simple trailing-append repair cannot fix.
        Returns None if nothing needed repair.
        """
        if not text:
            return None

        closing_map = {"{": "}", "[": "]"}
        output = []
        stack = []
        in_string = False
        escaped = False
        repaired_any = False

        for char in text:
            if in_string:
                output.append(char)
                if escaped:
                    escaped = False
                elif char == "\\":
                    escaped = True
                elif char == '"':
                    in_string = False
                continue

            if char == '"':
                in_string = True
                output.append(char)
                continue

            if char in "{[":
                stack.append(char)
                output.append(char)
                continue

            if char in "}]":
                # Insert any missing closers until the top of the stack
                # matches this closer, or the stack runs out entirely
                # (an unrepairable extra/unexpected closer).
                while stack and closing_map[stack[-1]] != char:
                    output.append(closing_map[stack.pop()])
                    repaired_any = True
                if stack:
                    stack.pop()
                output.append(char)
                continue

            output.append(char)

        if stack:
            for opener in reversed(stack):
                output.append(closing_map[opener])
            repaired_any = True

        if not repaired_any:
            return None

        return "".join(output)

    @staticmethod
    def is_empty_tool_response(response_text: str) -> bool:
        """Return whether the response explicitly contains an empty tool list."""
        if not response_text:
            return False
        try:
            parsed = json.loads(response_text.strip())
        except (TypeError, ValueError, json.JSONDecodeError):
            return False
        return isinstance(parsed, dict) and parsed.get("tool_calls") == []

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
            '{"type"',
            '"type": "function"',
            '<tool_call>',
            '<|tool_call',
        ]

        response_lower = response_text.lower()
        return any(indicator.lower() in response_lower for indicator in indicators)
