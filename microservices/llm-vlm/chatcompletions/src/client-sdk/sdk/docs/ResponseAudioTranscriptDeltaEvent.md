# ResponseAudioTranscriptDeltaEvent

Emitted when there is a partial transcript of audio.

## Properties

Name | Type | Description | Notes
------------ | ------------- | ------------- | -------------
**type** | **str** | The type of the event. Always &#x60;response.audio.transcript.delta&#x60;.  | 
**delta** | **str** | The partial transcript of the audio response.  | 

## Example

```python
from openapi_client.models.response_audio_transcript_delta_event import ResponseAudioTranscriptDeltaEvent

# TODO update the JSON string below
json = "{}"
# create an instance of ResponseAudioTranscriptDeltaEvent from a JSON string
response_audio_transcript_delta_event_instance = ResponseAudioTranscriptDeltaEvent.from_json(json)
# print the JSON string representation of the object
print(ResponseAudioTranscriptDeltaEvent.to_json())

# convert the object into a dict
response_audio_transcript_delta_event_dict = response_audio_transcript_delta_event_instance.to_dict()
# create an instance of ResponseAudioTranscriptDeltaEvent from a dict
response_audio_transcript_delta_event_from_dict = ResponseAudioTranscriptDeltaEvent.from_dict(response_audio_transcript_delta_event_dict)
```
[[Back to Model list]](../README.md#documentation-for-models) [[Back to API list]](../README.md#documentation-for-api-endpoints) [[Back to README]](../README.md)


