# ChatCompletionResponseMessageAnnotationsInnerUrlCitation

A URL citation when using web search.

## Properties

Name | Type | Description | Notes
------------ | ------------- | ------------- | -------------
**end_index** | **int** | The index of the last character of the URL citation in the message. | 
**start_index** | **int** | The index of the first character of the URL citation in the message. | 
**url** | **str** | The URL of the web resource. | 
**title** | **str** | The title of the web resource. | 

## Example

```python
from openapi_client.models.chat_completion_response_message_annotations_inner_url_citation import ChatCompletionResponseMessageAnnotationsInnerUrlCitation

# TODO update the JSON string below
json = "{}"
# create an instance of ChatCompletionResponseMessageAnnotationsInnerUrlCitation from a JSON string
chat_completion_response_message_annotations_inner_url_citation_instance = ChatCompletionResponseMessageAnnotationsInnerUrlCitation.from_json(json)
# print the JSON string representation of the object
print(ChatCompletionResponseMessageAnnotationsInnerUrlCitation.to_json())

# convert the object into a dict
chat_completion_response_message_annotations_inner_url_citation_dict = chat_completion_response_message_annotations_inner_url_citation_instance.to_dict()
# create an instance of ChatCompletionResponseMessageAnnotationsInnerUrlCitation from a dict
chat_completion_response_message_annotations_inner_url_citation_from_dict = ChatCompletionResponseMessageAnnotationsInnerUrlCitation.from_dict(chat_completion_response_message_annotations_inner_url_citation_dict)
```
[[Back to Model list]](../README.md#documentation-for-models) [[Back to API list]](../README.md#documentation-for-api-endpoints) [[Back to README]](../README.md)


