# ModifyAssistantRequest


## Properties

Name | Type | Description | Notes
------------ | ------------- | ------------- | -------------
**model** | [**ModifyAssistantRequestModel**](ModifyAssistantRequestModel.md) |  | [optional] 
**reasoning_effort** | [**ReasoningEffort**](ReasoningEffort.md) |  | [optional] [default to ReasoningEffort.MEDIUM]
**name** | **str** | The name of the assistant. The maximum length is 256 characters.  | [optional] 
**description** | **str** | The description of the assistant. The maximum length is 512 characters.  | [optional] 
**instructions** | **str** | The system instructions that the assistant uses. The maximum length is 256,000 characters.  | [optional] 
**tools** | [**List[AssistantObjectToolsInner]**](AssistantObjectToolsInner.md) | A list of tool enabled on the assistant. There can be a maximum of 128 tools per assistant. Tools can be of types &#x60;code_interpreter&#x60;, &#x60;file_search&#x60;, or &#x60;function&#x60;.  | [optional] [default to []]
**tool_resources** | [**ModifyAssistantRequestToolResources**](ModifyAssistantRequestToolResources.md) |  | [optional] 
**metadata** | **Dict[str, str]** | Set of 16 key-value pairs that can be attached to an object. This can be useful for storing additional information about the object in a structured format, and querying for objects via API or the dashboard.   Keys are strings with a maximum length of 64 characters. Values are strings with a maximum length of 512 characters.  | [optional] 
**temperature** | **float** | What sampling temperature to use, between 0 and 2. Higher values like 0.8 will make the output more random, while lower values like 0.2 will make it more focused and deterministic.  | [optional] [default to 1]
**top_p** | **float** | An alternative to sampling with temperature, called nucleus sampling, where the model considers the results of the tokens with top_p probability mass. So 0.1 means only the tokens comprising the top 10% probability mass are considered.  We generally recommend altering this or temperature but not both.  | [optional] [default to 1]
**response_format** | [**AssistantsApiResponseFormatOption**](AssistantsApiResponseFormatOption.md) |  | [optional] 

## Example

```python
from openapi_client.models.modify_assistant_request import ModifyAssistantRequest

# TODO update the JSON string below
json = "{}"
# create an instance of ModifyAssistantRequest from a JSON string
modify_assistant_request_instance = ModifyAssistantRequest.from_json(json)
# print the JSON string representation of the object
print(ModifyAssistantRequest.to_json())

# convert the object into a dict
modify_assistant_request_dict = modify_assistant_request_instance.to_dict()
# create an instance of ModifyAssistantRequest from a dict
modify_assistant_request_from_dict = ModifyAssistantRequest.from_dict(modify_assistant_request_dict)
```
[[Back to Model list]](../README.md#documentation-for-models) [[Back to API list]](../README.md#documentation-for-api-endpoints) [[Back to README]](../README.md)


