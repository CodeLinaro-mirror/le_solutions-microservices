# RealtimeServerEventOutputAudioBufferCleared

**WebRTC Only:** Emitted when the output audio buffer is cleared. This happens either in VAD mode when the user has interrupted (`input_audio_buffer.speech_started`), or when the client has emitted the `output_audio_buffer.clear` event to manually cut off the current audio response. [Learn more](/docs/guides/realtime-model-capabilities#client-and-server-events-for-audio-in-webrtc). 

## Properties

Name | Type | Description | Notes
------------ | ------------- | ------------- | -------------
**event_id** | **str** | The unique ID of the server event. | 
**type** | **str** | The event type, must be &#x60;output_audio_buffer.cleared&#x60;. | 
**response_id** | **str** | The unique ID of the response that produced the audio. | 

## Example

```python
from openapi_client.models.realtime_server_event_output_audio_buffer_cleared import RealtimeServerEventOutputAudioBufferCleared

# TODO update the JSON string below
json = "{}"
# create an instance of RealtimeServerEventOutputAudioBufferCleared from a JSON string
realtime_server_event_output_audio_buffer_cleared_instance = RealtimeServerEventOutputAudioBufferCleared.from_json(json)
# print the JSON string representation of the object
print(RealtimeServerEventOutputAudioBufferCleared.to_json())

# convert the object into a dict
realtime_server_event_output_audio_buffer_cleared_dict = realtime_server_event_output_audio_buffer_cleared_instance.to_dict()
# create an instance of RealtimeServerEventOutputAudioBufferCleared from a dict
realtime_server_event_output_audio_buffer_cleared_from_dict = RealtimeServerEventOutputAudioBufferCleared.from_dict(realtime_server_event_output_audio_buffer_cleared_dict)
```
[[Back to Model list]](../README.md#documentation-for-models) [[Back to API list]](../README.md#documentation-for-api-endpoints) [[Back to README]](../README.md)


