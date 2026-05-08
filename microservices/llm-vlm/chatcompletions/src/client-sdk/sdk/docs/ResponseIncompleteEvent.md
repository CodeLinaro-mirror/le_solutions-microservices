# ResponseIncompleteEvent

An event that is emitted when a response finishes as incomplete. 

## Properties

Name | Type | Description | Notes
------------ | ------------- | ------------- | -------------
**type** | **str** | The type of the event. Always &#x60;response.incomplete&#x60;.  | 
**response** | [**Response**](Response.md) |  | 

## Example

```python
from openapi_client.models.response_incomplete_event import ResponseIncompleteEvent

# TODO update the JSON string below
json = "{}"
# create an instance of ResponseIncompleteEvent from a JSON string
response_incomplete_event_instance = ResponseIncompleteEvent.from_json(json)
# print the JSON string representation of the object
print(ResponseIncompleteEvent.to_json())

# convert the object into a dict
response_incomplete_event_dict = response_incomplete_event_instance.to_dict()
# create an instance of ResponseIncompleteEvent from a dict
response_incomplete_event_from_dict = ResponseIncompleteEvent.from_dict(response_incomplete_event_dict)
```
[[Back to Model list]](../README.md#documentation-for-models) [[Back to API list]](../README.md#documentation-for-api-endpoints) [[Back to README]](../README.md)


