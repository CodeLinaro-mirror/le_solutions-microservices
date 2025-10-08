# ResponseProperties


## Properties

Name | Type | Description | Notes
------------ | ------------- | ------------- | -------------
**previous_response_id** | **str** | The unique ID of the previous response to the model. Use this to create multi-turn conversations. Learn more about  [conversation state](/docs/guides/conversation-state).  | [optional] 
**model** | [**ModelIdsResponses**](ModelIdsResponses.md) |  | [optional] 
**reasoning** | [**Reasoning**](Reasoning.md) |  | [optional] 
**max_output_tokens** | **int** | An upper bound for the number of tokens that can be generated for a response, including visible output tokens and [reasoning tokens](/docs/guides/reasoning).  | [optional] 
**instructions** | **str** | Inserts a system (or developer) message as the first item in the model&#39;s context.  When using along with &#x60;previous_response_id&#x60;, the instructions from a previous response will not be carried over to the next response. This makes it simple to swap out system (or developer) messages in new responses.  | [optional] 
**text** | [**ResponsePropertiesText**](ResponsePropertiesText.md) |  | [optional] 
**tools** | [**List[Tool]**](Tool.md) | An array of tools the model may call while generating a response. You  can specify which tool to use by setting the &#x60;tool_choice&#x60; parameter.  The two categories of tools you can provide the model are:  - **Built-in tools**: Tools that are provided by GenAI that extend the   model&#39;s capabilities, like [web search](/docs/guides/tools-web-search)   or [file search](/docs/guides/tools-file-search). Learn more about   [built-in tools](/docs/guides/tools). - **Function calls (custom tools)**: Functions that are defined by you,   enabling the model to call your own code. Learn more about   [function calling](/docs/guides/function-calling).  | [optional] 
**tool_choice** | [**ResponsePropertiesToolChoice**](ResponsePropertiesToolChoice.md) |  | [optional] 
**truncation** | **str** | The truncation strategy to use for the model response. - &#x60;auto&#x60;: If the context of this response and previous ones exceeds   the model&#39;s context window size, the model will truncate the    response to fit the context window by dropping input items in the   middle of the conversation.  - &#x60;disabled&#x60; (default): If a model response will exceed the context window    size for a model, the request will fail with a 400 error.  | [optional] [default to 'disabled']

## Example

```python
from openapi_client.models.response_properties import ResponseProperties

# TODO update the JSON string below
json = "{}"
# create an instance of ResponseProperties from a JSON string
response_properties_instance = ResponseProperties.from_json(json)
# print the JSON string representation of the object
print(ResponseProperties.to_json())

# convert the object into a dict
response_properties_dict = response_properties_instance.to_dict()
# create an instance of ResponseProperties from a dict
response_properties_from_dict = ResponseProperties.from_dict(response_properties_dict)
```
[[Back to Model list]](../README.md#documentation-for-models) [[Back to API list]](../README.md#documentation-for-api-endpoints) [[Back to README]](../README.md)


