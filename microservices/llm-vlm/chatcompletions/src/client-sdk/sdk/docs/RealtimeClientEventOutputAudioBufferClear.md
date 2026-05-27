# RealtimeClientEventOutputAudioBufferClear

**WebRTC Only:** Emit to cut off the current audio response. This will trigger the server to stop generating audio and emit a `output_audio_buffer.cleared` event. This  event should be preceded by a `response.cancel` client event to stop the  generation of the current response. [Learn more](/docs/guides/realtime-model-capabilities#client-and-server-events-for-audio-in-webrtc). 

## Properties

Name | Type | Description | Notes
------------ | ------------- | ------------- | -------------
**event_id** | **str** | The unique ID of the client event used for error handling. | [optional] 
**type** | **str** | The event type, must be &#x60;output_audio_buffer.clear&#x60;. | 

## Example

```python
from openapi_client.models.realtime_client_event_output_audio_buffer_clear import RealtimeClientEventOutputAudioBufferClear

# TODO update the JSON string below
json = "{}"
# create an instance of RealtimeClientEventOutputAudioBufferClear from a JSON string
realtime_client_event_output_audio_buffer_clear_instance = RealtimeClientEventOutputAudioBufferClear.from_json(json)
# print the JSON string representation of the object
print(RealtimeClientEventOutputAudioBufferClear.to_json())

# convert the object into a dict
realtime_client_event_output_audio_buffer_clear_dict = realtime_client_event_output_audio_buffer_clear_instance.to_dict()
# create an instance of RealtimeClientEventOutputAudioBufferClear from a dict
realtime_client_event_output_audio_buffer_clear_from_dict = RealtimeClientEventOutputAudioBufferClear.from_dict(realtime_client_event_output_audio_buffer_clear_dict)
```
[[Back to Model list]](../README.md#documentation-for-models) [[Back to API list]](../README.md#documentation-for-api-endpoints) [[Back to README]](../README.md)


