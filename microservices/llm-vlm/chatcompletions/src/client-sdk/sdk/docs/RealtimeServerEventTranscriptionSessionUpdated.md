# RealtimeServerEventTranscriptionSessionUpdated

Returned when a transcription session is updated with a `transcription_session.update` event, unless  there is an error. 

## Properties

Name | Type | Description | Notes
------------ | ------------- | ------------- | -------------
**event_id** | **str** | The unique ID of the server event. | 
**type** | **str** | The event type, must be &#x60;transcription_session.updated&#x60;. | 
**session** | [**RealtimeTranscriptionSessionCreateResponse**](RealtimeTranscriptionSessionCreateResponse.md) |  | 

## Example

```python
from openapi_client.models.realtime_server_event_transcription_session_updated import RealtimeServerEventTranscriptionSessionUpdated

# TODO update the JSON string below
json = "{}"
# create an instance of RealtimeServerEventTranscriptionSessionUpdated from a JSON string
realtime_server_event_transcription_session_updated_instance = RealtimeServerEventTranscriptionSessionUpdated.from_json(json)
# print the JSON string representation of the object
print(RealtimeServerEventTranscriptionSessionUpdated.to_json())

# convert the object into a dict
realtime_server_event_transcription_session_updated_dict = realtime_server_event_transcription_session_updated_instance.to_dict()
# create an instance of RealtimeServerEventTranscriptionSessionUpdated from a dict
realtime_server_event_transcription_session_updated_from_dict = RealtimeServerEventTranscriptionSessionUpdated.from_dict(realtime_server_event_transcription_session_updated_dict)
```
[[Back to Model list]](../README.md#documentation-for-models) [[Back to API list]](../README.md#documentation-for-api-endpoints) [[Back to README]](../README.md)


