# RealtimeServerEventOutputAudioBufferStopped

**WebRTC Only:** Emitted when the output audio buffer has been completely drained on the server, and no more audio is forthcoming. This event is emitted after the full response data has been sent to the client (`response.done`). [Learn more](/docs/guides/realtime-model-capabilities#client-and-server-events-for-audio-in-webrtc). 

## Properties

Name | Type | Description | Notes
------------ | ------------- | ------------- | -------------
**event_id** | **str** | The unique ID of the server event. | 
**type** | **str** | The event type, must be &#x60;output_audio_buffer.stopped&#x60;. | 
**response_id** | **str** | The unique ID of the response that produced the audio. | 

## Example

```python
from openapi_client.models.realtime_server_event_output_audio_buffer_stopped import RealtimeServerEventOutputAudioBufferStopped

# TODO update the JSON string below
json = "{}"
# create an instance of RealtimeServerEventOutputAudioBufferStopped from a JSON string
realtime_server_event_output_audio_buffer_stopped_instance = RealtimeServerEventOutputAudioBufferStopped.from_json(json)
# print the JSON string representation of the object
print(RealtimeServerEventOutputAudioBufferStopped.to_json())

# convert the object into a dict
realtime_server_event_output_audio_buffer_stopped_dict = realtime_server_event_output_audio_buffer_stopped_instance.to_dict()
# create an instance of RealtimeServerEventOutputAudioBufferStopped from a dict
realtime_server_event_output_audio_buffer_stopped_from_dict = RealtimeServerEventOutputAudioBufferStopped.from_dict(realtime_server_event_output_audio_buffer_stopped_dict)
```
[[Back to Model list]](../README.md#documentation-for-models) [[Back to API list]](../README.md#documentation-for-api-endpoints) [[Back to README]](../README.md)


