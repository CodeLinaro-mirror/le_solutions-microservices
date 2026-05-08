# CreateChatCompletionRequestAllOfResponseFormat

An object specifying the format that the model must output.  Setting to `{ \"type\": \"json_schema\", \"json_schema\": {...} }` enables Structured Outputs which ensures the model will match your supplied JSON schema. Learn more in the [Structured Outputs guide](/docs/guides/structured-outputs).  Setting to `{ \"type\": \"json_object\" }` enables the older JSON mode, which ensures the message the model generates is valid JSON. Using `json_schema` is preferred for models that support it. 

## Properties

Name | Type | Description | Notes
------------ | ------------- | ------------- | -------------
**type** | **str** | The type of response format being defined. Always &#x60;text&#x60;. | 
**json_schema** | [**JSONSchema**](JSONSchema.md) |  | 

## Example

```python
from openapi_client.models.create_chat_completion_request_all_of_response_format import CreateChatCompletionRequestAllOfResponseFormat

# TODO update the JSON string below
json = "{}"
# create an instance of CreateChatCompletionRequestAllOfResponseFormat from a JSON string
create_chat_completion_request_all_of_response_format_instance = CreateChatCompletionRequestAllOfResponseFormat.from_json(json)
# print the JSON string representation of the object
print(CreateChatCompletionRequestAllOfResponseFormat.to_json())

# convert the object into a dict
create_chat_completion_request_all_of_response_format_dict = create_chat_completion_request_all_of_response_format_instance.to_dict()
# create an instance of CreateChatCompletionRequestAllOfResponseFormat from a dict
create_chat_completion_request_all_of_response_format_from_dict = CreateChatCompletionRequestAllOfResponseFormat.from_dict(create_chat_completion_request_all_of_response_format_dict)
```
[[Back to Model list]](../README.md#documentation-for-models) [[Back to API list]](../README.md#documentation-for-api-endpoints) [[Back to README]](../README.md)


