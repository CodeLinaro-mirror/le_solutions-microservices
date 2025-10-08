# ResponseErrorEvent

Emitted when an error occurs.

## Properties

Name | Type | Description | Notes
------------ | ------------- | ------------- | -------------
**type** | **str** | The type of the event. Always &#x60;error&#x60;.  | 
**code** | **str** | The error code.  | 
**message** | **str** | The error message.  | 
**param** | **str** | The error parameter.  | 

## Example

```python
from openapi_client.models.response_error_event import ResponseErrorEvent

# TODO update the JSON string below
json = "{}"
# create an instance of ResponseErrorEvent from a JSON string
response_error_event_instance = ResponseErrorEvent.from_json(json)
# print the JSON string representation of the object
print(ResponseErrorEvent.to_json())

# convert the object into a dict
response_error_event_dict = response_error_event_instance.to_dict()
# create an instance of ResponseErrorEvent from a dict
response_error_event_from_dict = ResponseErrorEvent.from_dict(response_error_event_dict)
```
[[Back to Model list]](../README.md#documentation-for-models) [[Back to API list]](../README.md#documentation-for-api-endpoints) [[Back to README]](../README.md)


