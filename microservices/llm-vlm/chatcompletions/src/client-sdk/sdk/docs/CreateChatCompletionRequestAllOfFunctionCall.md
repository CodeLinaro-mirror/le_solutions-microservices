# CreateChatCompletionRequestAllOfFunctionCall

Deprecated in favor of `tool_choice`.  Controls which (if any) function is called by the model.  `none` means the model will not call a function and instead generates a message.  `auto` means the model can pick between generating a message or calling a function.  Specifying a particular function via `{\"name\": \"my_function\"}` forces the model to call that function.  `none` is the default when no functions are present. `auto` is the default if functions are present. 

## Properties

Name | Type | Description | Notes
------------ | ------------- | ------------- | -------------
**name** | **str** | The name of the function to call. | 

## Example

```python
from openapi_client.models.create_chat_completion_request_all_of_function_call import CreateChatCompletionRequestAllOfFunctionCall

# TODO update the JSON string below
json = "{}"
# create an instance of CreateChatCompletionRequestAllOfFunctionCall from a JSON string
create_chat_completion_request_all_of_function_call_instance = CreateChatCompletionRequestAllOfFunctionCall.from_json(json)
# print the JSON string representation of the object
print(CreateChatCompletionRequestAllOfFunctionCall.to_json())

# convert the object into a dict
create_chat_completion_request_all_of_function_call_dict = create_chat_completion_request_all_of_function_call_instance.to_dict()
# create an instance of CreateChatCompletionRequestAllOfFunctionCall from a dict
create_chat_completion_request_all_of_function_call_from_dict = CreateChatCompletionRequestAllOfFunctionCall.from_dict(create_chat_completion_request_all_of_function_call_dict)
```
[[Back to Model list]](../README.md#documentation-for-models) [[Back to API list]](../README.md#documentation-for-api-endpoints) [[Back to README]](../README.md)


