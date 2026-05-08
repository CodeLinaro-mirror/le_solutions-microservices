# ResponseRefusalDeltaEvent

Emitted when there is a partial refusal text.

## Properties

Name | Type | Description | Notes
------------ | ------------- | ------------- | -------------
**type** | **str** | The type of the event. Always &#x60;response.refusal.delta&#x60;.  | 
**item_id** | **str** | The ID of the output item that the refusal text is added to.  | 
**output_index** | **int** | The index of the output item that the refusal text is added to.  | 
**content_index** | **int** | The index of the content part that the refusal text is added to.  | 
**delta** | **str** | The refusal text that is added.  | 

## Example

```python
from openapi_client.models.response_refusal_delta_event import ResponseRefusalDeltaEvent

# TODO update the JSON string below
json = "{}"
# create an instance of ResponseRefusalDeltaEvent from a JSON string
response_refusal_delta_event_instance = ResponseRefusalDeltaEvent.from_json(json)
# print the JSON string representation of the object
print(ResponseRefusalDeltaEvent.to_json())

# convert the object into a dict
response_refusal_delta_event_dict = response_refusal_delta_event_instance.to_dict()
# create an instance of ResponseRefusalDeltaEvent from a dict
response_refusal_delta_event_from_dict = ResponseRefusalDeltaEvent.from_dict(response_refusal_delta_event_dict)
```
[[Back to Model list]](../README.md#documentation-for-models) [[Back to API list]](../README.md#documentation-for-api-endpoints) [[Back to README]](../README.md)


