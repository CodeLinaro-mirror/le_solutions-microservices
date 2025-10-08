# ResponseTextAnnotationDeltaEvent

Emitted when a text annotation is added.

## Properties

Name | Type | Description | Notes
------------ | ------------- | ------------- | -------------
**type** | **str** | The type of the event. Always &#x60;response.output_text.annotation.added&#x60;.  | 
**item_id** | **str** | The ID of the output item that the text annotation was added to.  | 
**output_index** | **int** | The index of the output item that the text annotation was added to.  | 
**content_index** | **int** | The index of the content part that the text annotation was added to.  | 
**annotation_index** | **int** | The index of the annotation that was added.  | 
**annotation** | [**Annotation**](Annotation.md) |  | 

## Example

```python
from openapi_client.models.response_text_annotation_delta_event import ResponseTextAnnotationDeltaEvent

# TODO update the JSON string below
json = "{}"
# create an instance of ResponseTextAnnotationDeltaEvent from a JSON string
response_text_annotation_delta_event_instance = ResponseTextAnnotationDeltaEvent.from_json(json)
# print the JSON string representation of the object
print(ResponseTextAnnotationDeltaEvent.to_json())

# convert the object into a dict
response_text_annotation_delta_event_dict = response_text_annotation_delta_event_instance.to_dict()
# create an instance of ResponseTextAnnotationDeltaEvent from a dict
response_text_annotation_delta_event_from_dict = ResponseTextAnnotationDeltaEvent.from_dict(response_text_annotation_delta_event_dict)
```
[[Back to Model list]](../README.md#documentation-for-models) [[Back to API list]](../README.md#documentation-for-api-endpoints) [[Back to README]](../README.md)


