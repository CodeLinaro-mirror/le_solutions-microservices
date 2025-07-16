# RealtimeTranscriptionSessionCreateRequestInputAudioTranscription

Configuration for input audio transcription. The client can optionally set the language and prompt for transcription, these offer additional guidance to the transcription service. 

## Properties

Name | Type | Description | Notes
------------ | ------------- | ------------- | -------------
**model** | **str** | The model to use for transcription.  | [optional] 
**language** | **str** | The language of the input audio. Supplying the input language in ISO-639-1 (e.g. &#x60;en&#x60;) format will improve accuracy and latency.  | [optional] 
**prompt** | **str** | An optional text to guide the model&#39;s style or continue a previous audio segment. The prompt is a free text string, for example \&quot;expect words related to technology\&quot;.  | [optional] 

## Example

```python
from openapi_client.models.realtime_transcription_session_create_request_input_audio_transcription import RealtimeTranscriptionSessionCreateRequestInputAudioTranscription

# TODO update the JSON string below
json = "{}"
# create an instance of RealtimeTranscriptionSessionCreateRequestInputAudioTranscription from a JSON string
realtime_transcription_session_create_request_input_audio_transcription_instance = RealtimeTranscriptionSessionCreateRequestInputAudioTranscription.from_json(json)
# print the JSON string representation of the object
print(RealtimeTranscriptionSessionCreateRequestInputAudioTranscription.to_json())

# convert the object into a dict
realtime_transcription_session_create_request_input_audio_transcription_dict = realtime_transcription_session_create_request_input_audio_transcription_instance.to_dict()
# create an instance of RealtimeTranscriptionSessionCreateRequestInputAudioTranscription from a dict
realtime_transcription_session_create_request_input_audio_transcription_from_dict = RealtimeTranscriptionSessionCreateRequestInputAudioTranscription.from_dict(realtime_transcription_session_create_request_input_audio_transcription_dict)
```
[[Back to Model list]](../README.md#documentation-for-models) [[Back to API list]](../README.md#documentation-for-api-endpoints) [[Back to README]](../README.md)


