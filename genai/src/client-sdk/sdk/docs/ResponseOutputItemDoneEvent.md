# ResponseOutputItemDoneEvent

Emitted when an output item is marked done.

## Properties

Name | Type | Description | Notes
------------ | ------------- | ------------- | -------------
**type** | **str** | The type of the event. Always &#x60;response.output_item.done&#x60;.  | 
**output_index** | **int** | The index of the output item that was marked done.  | 
**item** | [**OutputItem**](OutputItem.md) |  | 

## Example

```python
from openapi_client.models.response_output_item_done_event import ResponseOutputItemDoneEvent

# TODO update the JSON string below
json = "{}"
# create an instance of ResponseOutputItemDoneEvent from a JSON string
response_output_item_done_event_instance = ResponseOutputItemDoneEvent.from_json(json)
# print the JSON string representation of the object
print(ResponseOutputItemDoneEvent.to_json())

# convert the object into a dict
response_output_item_done_event_dict = response_output_item_done_event_instance.to_dict()
# create an instance of ResponseOutputItemDoneEvent from a dict
response_output_item_done_event_from_dict = ResponseOutputItemDoneEvent.from_dict(response_output_item_done_event_dict)
```
[[Back to Model list]](../README.md#documentation-for-models) [[Back to API list]](../README.md#documentation-for-api-endpoints) [[Back to README]](../README.md)


