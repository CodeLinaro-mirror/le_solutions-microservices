# RealtimeSessionInputAudioTranscription

Configuration for input audio transcription, defaults to off and can be  set to `null` to turn off once on. Input audio transcription is not native to the model, since the model consumes audio directly. Transcription runs  asynchronously through [the /audio/transcriptions endpoint] and should be treated as guidance of input audio content rather than precisely what the model heard. The client can optionally set the language and prompt for transcription, these offer additional guidance to the transcription service. 

## Properties

Name | Type | Description | Notes
------------ | ------------- | ------------- | -------------
**model** | **str** | The model to use for transcription&#x60;.  | [optional] 
**language** | **str** | The language of the input audio. Supplying the input language in ISO-639-1 (e.g. &#x60;en&#x60;) format will improve accuracy and latency.  | [optional] 
**prompt** | **str** | An optional text to guide the model&#39;s style or continue a previous audio segment.  | [optional] 

## Example

```python
from openapi_client.models.realtime_session_input_audio_transcription import RealtimeSessionInputAudioTranscription

# TODO update the JSON string below
json = "{}"
# create an instance of RealtimeSessionInputAudioTranscription from a JSON string
realtime_session_input_audio_transcription_instance = RealtimeSessionInputAudioTranscription.from_json(json)
# print the JSON string representation of the object
print(RealtimeSessionInputAudioTranscription.to_json())

# convert the object into a dict
realtime_session_input_audio_transcription_dict = realtime_session_input_audio_transcription_instance.to_dict()
# create an instance of RealtimeSessionInputAudioTranscription from a dict
realtime_session_input_audio_transcription_from_dict = RealtimeSessionInputAudioTranscription.from_dict(realtime_session_input_audio_transcription_dict)
```
[[Back to Model list]](../README.md#documentation-for-models) [[Back to API list]](../README.md#documentation-for-api-endpoints) [[Back to README]](../README.md)


