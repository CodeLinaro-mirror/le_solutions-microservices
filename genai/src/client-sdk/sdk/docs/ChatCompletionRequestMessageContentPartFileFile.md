# ChatCompletionRequestMessageContentPartFileFile


## Properties

Name | Type | Description | Notes
------------ | ------------- | ------------- | -------------
**filename** | **str** | The name of the file, used when passing the file to the model as a  string.  | [optional] 
**file_data** | **str** | The base64 encoded file data, used when passing the file to the model  as a string.  | [optional] 
**file_id** | **str** | The ID of an uploaded file to use as input.  | [optional] 

## Example

```python
from openapi_client.models.chat_completion_request_message_content_part_file_file import ChatCompletionRequestMessageContentPartFileFile

# TODO update the JSON string below
json = "{}"
# create an instance of ChatCompletionRequestMessageContentPartFileFile from a JSON string
chat_completion_request_message_content_part_file_file_instance = ChatCompletionRequestMessageContentPartFileFile.from_json(json)
# print the JSON string representation of the object
print(ChatCompletionRequestMessageContentPartFileFile.to_json())

# convert the object into a dict
chat_completion_request_message_content_part_file_file_dict = chat_completion_request_message_content_part_file_file_instance.to_dict()
# create an instance of ChatCompletionRequestMessageContentPartFileFile from a dict
chat_completion_request_message_content_part_file_file_from_dict = ChatCompletionRequestMessageContentPartFileFile.from_dict(chat_completion_request_message_content_part_file_file_dict)
```
[[Back to Model list]](../README.md#documentation-for-models) [[Back to API list]](../README.md#documentation-for-api-endpoints) [[Back to README]](../README.md)


