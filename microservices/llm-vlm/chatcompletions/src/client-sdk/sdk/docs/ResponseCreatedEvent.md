# ResponseCreatedEvent

An event that is emitted when a response is created. 

## Properties

Name | Type | Description | Notes
------------ | ------------- | ------------- | -------------
**type** | **str** | The type of the event. Always &#x60;response.created&#x60;.  | 
**response** | [**Response**](Response.md) |  | 

## Example

```python
from openapi_client.models.response_created_event import ResponseCreatedEvent

# TODO update the JSON string below
json = "{}"
# create an instance of ResponseCreatedEvent from a JSON string
response_created_event_instance = ResponseCreatedEvent.from_json(json)
# print the JSON string representation of the object
print(ResponseCreatedEvent.to_json())

# convert the object into a dict
response_created_event_dict = response_created_event_instance.to_dict()
# create an instance of ResponseCreatedEvent from a dict
response_created_event_from_dict = ResponseCreatedEvent.from_dict(response_created_event_dict)
```
[[Back to Model list]](../README.md#documentation-for-models) [[Back to API list]](../README.md#documentation-for-api-endpoints) [[Back to README]](../README.md)


