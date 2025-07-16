# ResponseContentPartDoneEvent

Emitted when a content part is done.

## Properties

Name | Type | Description | Notes
------------ | ------------- | ------------- | -------------
**type** | **str** | The type of the event. Always &#x60;response.content_part.done&#x60;.  | 
**item_id** | **str** | The ID of the output item that the content part was added to.  | 
**output_index** | **int** | The index of the output item that the content part was added to.  | 
**content_index** | **int** | The index of the content part that is done.  | 
**part** | [**OutputContent**](OutputContent.md) |  | 

## Example

```python
from openapi_client.models.response_content_part_done_event import ResponseContentPartDoneEvent

# TODO update the JSON string below
json = "{}"
# create an instance of ResponseContentPartDoneEvent from a JSON string
response_content_part_done_event_instance = ResponseContentPartDoneEvent.from_json(json)
# print the JSON string representation of the object
print(ResponseContentPartDoneEvent.to_json())

# convert the object into a dict
response_content_part_done_event_dict = response_content_part_done_event_instance.to_dict()
# create an instance of ResponseContentPartDoneEvent from a dict
response_content_part_done_event_from_dict = ResponseContentPartDoneEvent.from_dict(response_content_part_done_event_dict)
```
[[Back to Model list]](../README.md#documentation-for-models) [[Back to API list]](../README.md#documentation-for-api-endpoints) [[Back to README]](../README.md)


