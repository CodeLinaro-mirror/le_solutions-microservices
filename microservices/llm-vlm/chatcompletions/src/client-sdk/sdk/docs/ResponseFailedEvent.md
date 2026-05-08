# ResponseFailedEvent

An event that is emitted when a response fails. 

## Properties

Name | Type | Description | Notes
------------ | ------------- | ------------- | -------------
**type** | **str** | The type of the event. Always &#x60;response.failed&#x60;.  | 
**response** | [**Response**](Response.md) |  | 

## Example

```python
from openapi_client.models.response_failed_event import ResponseFailedEvent

# TODO update the JSON string below
json = "{}"
# create an instance of ResponseFailedEvent from a JSON string
response_failed_event_instance = ResponseFailedEvent.from_json(json)
# print the JSON string representation of the object
print(ResponseFailedEvent.to_json())

# convert the object into a dict
response_failed_event_dict = response_failed_event_instance.to_dict()
# create an instance of ResponseFailedEvent from a dict
response_failed_event_from_dict = ResponseFailedEvent.from_dict(response_failed_event_dict)
```
[[Back to Model list]](../README.md#documentation-for-models) [[Back to API list]](../README.md#documentation-for-api-endpoints) [[Back to README]](../README.md)


