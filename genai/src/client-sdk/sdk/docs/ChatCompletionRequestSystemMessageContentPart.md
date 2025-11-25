# ChatCompletionRequestSystemMessageContentPart


## Properties

Name | Type | Description | Notes
------------ | ------------- | ------------- | -------------
**type** | **str** | The type of the content part. | 
**text** | **str** | The text content. | 

## Example

```python
from openapi_client.models.chat_completion_request_system_message_content_part import ChatCompletionRequestSystemMessageContentPart

# TODO update the JSON string below
json = "{}"
# create an instance of ChatCompletionRequestSystemMessageContentPart from a JSON string
chat_completion_request_system_message_content_part_instance = ChatCompletionRequestSystemMessageContentPart.from_json(json)
# print the JSON string representation of the object
print(ChatCompletionRequestSystemMessageContentPart.to_json())

# convert the object into a dict
chat_completion_request_system_message_content_part_dict = chat_completion_request_system_message_content_part_instance.to_dict()
# create an instance of ChatCompletionRequestSystemMessageContentPart from a dict
chat_completion_request_system_message_content_part_from_dict = ChatCompletionRequestSystemMessageContentPart.from_dict(chat_completion_request_system_message_content_part_dict)
```
[[Back to Model list]](../README.md#documentation-for-models) [[Back to API list]](../README.md#documentation-for-api-endpoints) [[Back to README]](../README.md)


