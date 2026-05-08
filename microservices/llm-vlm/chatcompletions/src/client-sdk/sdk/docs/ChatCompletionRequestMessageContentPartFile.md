# ChatCompletionRequestMessageContentPartFile

Learn about [file inputs](/docs/guides/text) for text generation. 

## Properties

Name | Type | Description | Notes
------------ | ------------- | ------------- | -------------
**type** | **str** | The type of the content part. Always &#x60;file&#x60;. | 
**file** | [**ChatCompletionRequestMessageContentPartFileFile**](ChatCompletionRequestMessageContentPartFileFile.md) |  | 

## Example

```python
from openapi_client.models.chat_completion_request_message_content_part_file import ChatCompletionRequestMessageContentPartFile

# TODO update the JSON string below
json = "{}"
# create an instance of ChatCompletionRequestMessageContentPartFile from a JSON string
chat_completion_request_message_content_part_file_instance = ChatCompletionRequestMessageContentPartFile.from_json(json)
# print the JSON string representation of the object
print(ChatCompletionRequestMessageContentPartFile.to_json())

# convert the object into a dict
chat_completion_request_message_content_part_file_dict = chat_completion_request_message_content_part_file_instance.to_dict()
# create an instance of ChatCompletionRequestMessageContentPartFile from a dict
chat_completion_request_message_content_part_file_from_dict = ChatCompletionRequestMessageContentPartFile.from_dict(chat_completion_request_message_content_part_file_dict)
```
[[Back to Model list]](../README.md#documentation-for-models) [[Back to API list]](../README.md#documentation-for-api-endpoints) [[Back to README]](../README.md)


