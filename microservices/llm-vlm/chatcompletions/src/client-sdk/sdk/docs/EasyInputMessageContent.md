# EasyInputMessageContent

Text, image, or audio input to the model, used to generate a response. Can also contain previous assistant responses. 

## Properties

Name | Type | Description | Notes
------------ | ------------- | ------------- | -------------

## Example

```python
from openapi_client.models.easy_input_message_content import EasyInputMessageContent

# TODO update the JSON string below
json = "{}"
# create an instance of EasyInputMessageContent from a JSON string
easy_input_message_content_instance = EasyInputMessageContent.from_json(json)
# print the JSON string representation of the object
print(EasyInputMessageContent.to_json())

# convert the object into a dict
easy_input_message_content_dict = easy_input_message_content_instance.to_dict()
# create an instance of EasyInputMessageContent from a dict
easy_input_message_content_from_dict = EasyInputMessageContent.from_dict(easy_input_message_content_dict)
```
[[Back to Model list]](../README.md#documentation-for-models) [[Back to API list]](../README.md#documentation-for-api-endpoints) [[Back to README]](../README.md)


