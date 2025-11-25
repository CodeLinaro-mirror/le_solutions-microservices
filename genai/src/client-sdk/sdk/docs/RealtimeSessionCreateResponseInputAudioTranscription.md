# RealtimeSessionCreateResponseInputAudioTranscription

Configuration for input audio transcription, defaults to off and can be  set to `null` to turn off once on. Input audio transcription is not native  to the model, since the model consumes audio directly. Transcription runs  asynchronously and should be treated as rough guidance  rather than the representation understood by the model. 

## Properties

Name | Type | Description | Notes
------------ | ------------- | ------------- | -------------
**model** | **str** | The model to use for transcription.  | [optional] 

## Example

```python
from openapi_client.models.realtime_session_create_response_input_audio_transcription import RealtimeSessionCreateResponseInputAudioTranscription

# TODO update the JSON string below
json = "{}"
# create an instance of RealtimeSessionCreateResponseInputAudioTranscription from a JSON string
realtime_session_create_response_input_audio_transcription_instance = RealtimeSessionCreateResponseInputAudioTranscription.from_json(json)
# print the JSON string representation of the object
print(RealtimeSessionCreateResponseInputAudioTranscription.to_json())

# convert the object into a dict
realtime_session_create_response_input_audio_transcription_dict = realtime_session_create_response_input_audio_transcription_instance.to_dict()
# create an instance of RealtimeSessionCreateResponseInputAudioTranscription from a dict
realtime_session_create_response_input_audio_transcription_from_dict = RealtimeSessionCreateResponseInputAudioTranscription.from_dict(realtime_session_create_response_input_audio_transcription_dict)
```
[[Back to Model list]](../README.md#documentation-for-models) [[Back to API list]](../README.md#documentation-for-api-endpoints) [[Back to README]](../README.md)


