# ResponseTextDeltaEvent

Emitted when there is an additional text delta.

## Properties

Name | Type | Description | Notes
------------ | ------------- | ------------- | -------------
**type** | **str** | The type of the event. Always &#x60;response.output_text.delta&#x60;.  | 
**item_id** | **str** | The ID of the output item that the text delta was added to.  | 
**output_index** | **int** | The index of the output item that the text delta was added to.  | 
**content_index** | **int** | The index of the content part that the text delta was added to.  | 
**delta** | **str** | The text delta that was added.  | 

## Example

```python
from openapi_client.models.response_text_delta_event import ResponseTextDeltaEvent

# TODO update the JSON string below
json = "{}"
# create an instance of ResponseTextDeltaEvent from a JSON string
response_text_delta_event_instance = ResponseTextDeltaEvent.from_json(json)
# print the JSON string representation of the object
print(ResponseTextDeltaEvent.to_json())

# convert the object into a dict
response_text_delta_event_dict = response_text_delta_event_instance.to_dict()
# create an instance of ResponseTextDeltaEvent from a dict
response_text_delta_event_from_dict = ResponseTextDeltaEvent.from_dict(response_text_delta_event_dict)
```
[[Back to Model list]](../README.md#documentation-for-models) [[Back to API list]](../README.md#documentation-for-api-endpoints) [[Back to README]](../README.md)


