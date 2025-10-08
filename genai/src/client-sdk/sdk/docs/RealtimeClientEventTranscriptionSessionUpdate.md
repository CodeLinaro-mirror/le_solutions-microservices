# RealtimeClientEventTranscriptionSessionUpdate

Send this event to update a transcription session. 

## Properties

Name | Type | Description | Notes
------------ | ------------- | ------------- | -------------
**event_id** | **str** | Optional client-generated ID used to identify this event. | [optional] 
**type** | **str** | The event type, must be &#x60;transcription_session.update&#x60;. | 
**session** | [**RealtimeTranscriptionSessionCreateRequest**](RealtimeTranscriptionSessionCreateRequest.md) |  | 

## Example

```python
from openapi_client.models.realtime_client_event_transcription_session_update import RealtimeClientEventTranscriptionSessionUpdate

# TODO update the JSON string below
json = "{}"
# create an instance of RealtimeClientEventTranscriptionSessionUpdate from a JSON string
realtime_client_event_transcription_session_update_instance = RealtimeClientEventTranscriptionSessionUpdate.from_json(json)
# print the JSON string representation of the object
print(RealtimeClientEventTranscriptionSessionUpdate.to_json())

# convert the object into a dict
realtime_client_event_transcription_session_update_dict = realtime_client_event_transcription_session_update_instance.to_dict()
# create an instance of RealtimeClientEventTranscriptionSessionUpdate from a dict
realtime_client_event_transcription_session_update_from_dict = RealtimeClientEventTranscriptionSessionUpdate.from_dict(realtime_client_event_transcription_session_update_dict)
```
[[Back to Model list]](../README.md#documentation-for-models) [[Back to API list]](../README.md#documentation-for-api-endpoints) [[Back to README]](../README.md)


