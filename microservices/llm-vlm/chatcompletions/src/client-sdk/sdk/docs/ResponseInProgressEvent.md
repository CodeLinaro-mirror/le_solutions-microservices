# ResponseInProgressEvent

Emitted when the response is in progress.

## Properties

Name | Type | Description | Notes
------------ | ------------- | ------------- | -------------
**type** | **str** | The type of the event. Always &#x60;response.in_progress&#x60;.  | 
**response** | [**Response**](Response.md) |  | 

## Example

```python
from openapi_client.models.response_in_progress_event import ResponseInProgressEvent

# TODO update the JSON string below
json = "{}"
# create an instance of ResponseInProgressEvent from a JSON string
response_in_progress_event_instance = ResponseInProgressEvent.from_json(json)
# print the JSON string representation of the object
print(ResponseInProgressEvent.to_json())

# convert the object into a dict
response_in_progress_event_dict = response_in_progress_event_instance.to_dict()
# create an instance of ResponseInProgressEvent from a dict
response_in_progress_event_from_dict = ResponseInProgressEvent.from_dict(response_in_progress_event_dict)
```
[[Back to Model list]](../README.md#documentation-for-models) [[Back to API list]](../README.md#documentation-for-api-endpoints) [[Back to README]](../README.md)


