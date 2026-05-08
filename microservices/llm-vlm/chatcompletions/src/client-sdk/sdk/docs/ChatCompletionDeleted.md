# ChatCompletionDeleted


## Properties

Name | Type | Description | Notes
------------ | ------------- | ------------- | -------------
**object** | **str** | The type of object being deleted. | 
**id** | **str** | The ID of the chat completion that was deleted. | 
**deleted** | **bool** | Whether the chat completion was deleted. | 

## Example

```python
from openapi_client.models.chat_completion_deleted import ChatCompletionDeleted

# TODO update the JSON string below
json = "{}"
# create an instance of ChatCompletionDeleted from a JSON string
chat_completion_deleted_instance = ChatCompletionDeleted.from_json(json)
# print the JSON string representation of the object
print(ChatCompletionDeleted.to_json())

# convert the object into a dict
chat_completion_deleted_dict = chat_completion_deleted_instance.to_dict()
# create an instance of ChatCompletionDeleted from a dict
chat_completion_deleted_from_dict = ChatCompletionDeleted.from_dict(chat_completion_deleted_dict)
```
[[Back to Model list]](../README.md#documentation-for-models) [[Back to API list]](../README.md#documentation-for-api-endpoints) [[Back to README]](../README.md)


