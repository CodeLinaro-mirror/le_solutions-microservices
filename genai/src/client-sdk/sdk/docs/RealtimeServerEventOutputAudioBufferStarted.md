# RealtimeServerEventOutputAudioBufferStarted

**WebRTC Only:** Emitted when the server begins streaming audio to the client. This event is emitted after an audio content part has been added (`response.content_part.added`) to the response. [Learn more](/docs/guides/realtime-model-capabilities#client-and-server-events-for-audio-in-webrtc). 

## Properties

Name | Type | Description | Notes
------------ | ------------- | ------------- | -------------
**event_id** | **str** | The unique ID of the server event. | 
**type** | **str** | The event type, must be &#x60;output_audio_buffer.started&#x60;. | 
**response_id** | **str** | The unique ID of the response that produced the audio. | 

## Example

```python
from openapi_client.models.realtime_server_event_output_audio_buffer_started import RealtimeServerEventOutputAudioBufferStarted

# TODO update the JSON string below
json = "{}"
# create an instance of RealtimeServerEventOutputAudioBufferStarted from a JSON string
realtime_server_event_output_audio_buffer_started_instance = RealtimeServerEventOutputAudioBufferStarted.from_json(json)
# print the JSON string representation of the object
print(RealtimeServerEventOutputAudioBufferStarted.to_json())

# convert the object into a dict
realtime_server_event_output_audio_buffer_started_dict = realtime_server_event_output_audio_buffer_started_instance.to_dict()
# create an instance of RealtimeServerEventOutputAudioBufferStarted from a dict
realtime_server_event_output_audio_buffer_started_from_dict = RealtimeServerEventOutputAudioBufferStarted.from_dict(realtime_server_event_output_audio_buffer_started_dict)
```
[[Back to Model list]](../README.md#documentation-for-models) [[Back to API list]](../README.md#documentation-for-api-endpoints) [[Back to README]](../README.md)


