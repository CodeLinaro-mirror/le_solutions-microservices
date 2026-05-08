# ChatCompletionList

An object representing a list of Chat Completions. 

## Properties

Name | Type | Description | Notes
------------ | ------------- | ------------- | -------------
**object** | **str** | The type of this object. It is always set to \&quot;list\&quot;.  | [default to 'list']
**data** | [**List[CreateChatCompletionResponse]**](CreateChatCompletionResponse.md) | An array of chat completion objects.  | 
**first_id** | **str** | The identifier of the first chat completion in the data array. | 
**last_id** | **str** | The identifier of the last chat completion in the data array. | 
**has_more** | **bool** | Indicates whether there are more Chat Completions available. | 

## Example

```python
from openapi_client.models.chat_completion_list import ChatCompletionList

# TODO update the JSON string below
json = "{}"
# create an instance of ChatCompletionList from a JSON string
chat_completion_list_instance = ChatCompletionList.from_json(json)
# print the JSON string representation of the object
print(ChatCompletionList.to_json())

# convert the object into a dict
chat_completion_list_dict = chat_completion_list_instance.to_dict()
# create an instance of ChatCompletionList from a dict
chat_completion_list_from_dict = ChatCompletionList.from_dict(chat_completion_list_dict)
```
[[Back to Model list]](../README.md#documentation-for-models) [[Back to API list]](../README.md#documentation-for-api-endpoints) [[Back to README]](../README.md)


