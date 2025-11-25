# ChatCompletionMessageList

An object representing a list of chat completion messages. 

## Properties

Name | Type | Description | Notes
------------ | ------------- | ------------- | -------------
**object** | **str** | The type of this object. It is always set to \&quot;list\&quot;.  | [default to 'list']
**data** | [**List[ChatCompletionMessageListDataInner]**](ChatCompletionMessageListDataInner.md) | An array of chat completion message objects.  | 
**first_id** | **str** | The identifier of the first chat message in the data array. | 
**last_id** | **str** | The identifier of the last chat message in the data array. | 
**has_more** | **bool** | Indicates whether there are more chat messages available. | 

## Example

```python
from openapi_client.models.chat_completion_message_list import ChatCompletionMessageList

# TODO update the JSON string below
json = "{}"
# create an instance of ChatCompletionMessageList from a JSON string
chat_completion_message_list_instance = ChatCompletionMessageList.from_json(json)
# print the JSON string representation of the object
print(ChatCompletionMessageList.to_json())

# convert the object into a dict
chat_completion_message_list_dict = chat_completion_message_list_instance.to_dict()
# create an instance of ChatCompletionMessageList from a dict
chat_completion_message_list_from_dict = ChatCompletionMessageList.from_dict(chat_completion_message_list_dict)
```
[[Back to Model list]](../README.md#documentation-for-models) [[Back to API list]](../README.md#documentation-for-api-endpoints) [[Back to README]](../README.md)


