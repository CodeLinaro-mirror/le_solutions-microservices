# ResponseCompletedEvent

Emitted when the model response is complete.

## Properties

Name | Type | Description | Notes
------------ | ------------- | ------------- | -------------
**type** | **str** | The type of the event. Always &#x60;response.completed&#x60;.  | 
**response** | [**Response**](Response.md) |  | 

## Example

```python
from openapi_client.models.response_completed_event import ResponseCompletedEvent

# TODO update the JSON string below
json = "{}"
# create an instance of ResponseCompletedEvent from a JSON string
response_completed_event_instance = ResponseCompletedEvent.from_json(json)
# print the JSON string representation of the object
print(ResponseCompletedEvent.to_json())

# convert the object into a dict
response_completed_event_dict = response_completed_event_instance.to_dict()
# create an instance of ResponseCompletedEvent from a dict
response_completed_event_from_dict = ResponseCompletedEvent.from_dict(response_completed_event_dict)
```
[[Back to Model list]](../README.md#documentation-for-models) [[Back to API list]](../README.md#documentation-for-api-endpoints) [[Back to README]](../README.md)


