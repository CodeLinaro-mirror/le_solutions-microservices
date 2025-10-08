# ResponseOutputItemAddedEvent

Emitted when a new output item is added.

## Properties

Name | Type | Description | Notes
------------ | ------------- | ------------- | -------------
**type** | **str** | The type of the event. Always &#x60;response.output_item.added&#x60;.  | 
**output_index** | **int** | The index of the output item that was added.  | 
**item** | [**OutputItem**](OutputItem.md) |  | 

## Example

```python
from openapi_client.models.response_output_item_added_event import ResponseOutputItemAddedEvent

# TODO update the JSON string below
json = "{}"
# create an instance of ResponseOutputItemAddedEvent from a JSON string
response_output_item_added_event_instance = ResponseOutputItemAddedEvent.from_json(json)
# print the JSON string representation of the object
print(ResponseOutputItemAddedEvent.to_json())

# convert the object into a dict
response_output_item_added_event_dict = response_output_item_added_event_instance.to_dict()
# create an instance of ResponseOutputItemAddedEvent from a dict
response_output_item_added_event_from_dict = ResponseOutputItemAddedEvent.from_dict(response_output_item_added_event_dict)
```
[[Back to Model list]](../README.md#documentation-for-models) [[Back to API list]](../README.md#documentation-for-api-endpoints) [[Back to README]](../README.md)


