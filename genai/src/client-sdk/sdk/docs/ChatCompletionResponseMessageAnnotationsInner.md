# ChatCompletionResponseMessageAnnotationsInner

A URL citation when using web search. 

## Properties

Name | Type | Description | Notes
------------ | ------------- | ------------- | -------------
**type** | **str** | The type of the URL citation. Always &#x60;url_citation&#x60;. | 
**url_citation** | [**ChatCompletionResponseMessageAnnotationsInnerUrlCitation**](ChatCompletionResponseMessageAnnotationsInnerUrlCitation.md) |  | 

## Example

```python
from openapi_client.models.chat_completion_response_message_annotations_inner import ChatCompletionResponseMessageAnnotationsInner

# TODO update the JSON string below
json = "{}"
# create an instance of ChatCompletionResponseMessageAnnotationsInner from a JSON string
chat_completion_response_message_annotations_inner_instance = ChatCompletionResponseMessageAnnotationsInner.from_json(json)
# print the JSON string representation of the object
print(ChatCompletionResponseMessageAnnotationsInner.to_json())

# convert the object into a dict
chat_completion_response_message_annotations_inner_dict = chat_completion_response_message_annotations_inner_instance.to_dict()
# create an instance of ChatCompletionResponseMessageAnnotationsInner from a dict
chat_completion_response_message_annotations_inner_from_dict = ChatCompletionResponseMessageAnnotationsInner.from_dict(chat_completion_response_message_annotations_inner_dict)
```
[[Back to Model list]](../README.md#documentation-for-models) [[Back to API list]](../README.md#documentation-for-api-endpoints) [[Back to README]](../README.md)


